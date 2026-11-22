// SPDX-License-Identifier: MIT
//
// The coroutine half of `PermissionService` (docs/specs/permissions-v1.md section 5 rule 1): the
// `runtime::Task` wrapper over `submit`, and the awaiter it suspends on.
//
// Its own translation unit for the file-size rule, and the split is the one the code already had:
// `permission_service.cpp` is the state machine and the four "do not ask" rules, this is the async
// shape. It follows `NetClient::send`'s pattern and for the same reasons, which are worth restating
// because each is a bug that was live first:
//
//   * The task's own token is deliberately empty. Cancellation here is a *value*
//   (`PermissionResult`
//     with a cancelled state), and a task token would make the runtime throw `TaskCancelled` at the
//     `co_await` instead of returning one.
//   * The awaiter's `await_ready()` is false even when the answer already arrived, so the coroutine
//     resumes from the completion queue rather than from the resolving stack. That is what makes
//     "answered by policy" and "answered by the OS" indistinguishable to a caller, which is the
//     whole point of applying the four rules inside `submit`.
//   * `abandoned` is set in the awaiter's destructor, so a caller that drops the task it was
//   awaiting
//     is never resumed by a completion that is already queued.

#include "tessera/appfw/permission_service.hpp"

#include <coroutine>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

namespace tessera::appfw {
namespace {

// The state a suspended `request` body waits for.
struct PermissionAwaitState {
    std::optional<PermissionResult> result;
    std::coroutine_handle<> handle;
    bool resume_posted = false;
    bool abandoned = false;
};

// The awaiter `request_body` suspends on.
class PermissionAwaiter {
public:
    PermissionAwaiter(runtime::CompletionQueue& completions,
                      std::shared_ptr<PermissionAwaitState> state)
        : completions_(&completions),
          state_(std::move(state)) {}

    // A destroyed frame must not be resumed by a completion that is already queued.
    ~PermissionAwaiter() {
        if (state_ != nullptr) {
            state_->abandoned = true;
        }
    }

    PermissionAwaiter(const PermissionAwaiter&) = delete;
    PermissionAwaiter& operator=(const PermissionAwaiter&) = delete;

    // Always false, even when the answer already arrived: `await_resume` has not run yet, and
    // posting the resume (below) keeps the coroutine off the resolving stack. Static because it
    // reads no member, which is also how the language's `await_ready` protocol allows it.
    [[nodiscard]] static bool await_ready() noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) const {
        state_->handle = handle;
        if (!state_->result.has_value() || state_->resume_posted) {
            return;
        }
        state_->resume_posted = true;
        (void)completions_->post(runtime::CompletionPriority::Visible,
                                 [state = state_]() { resume_if_live(*state); });
    }

    // The one place a coroutine frame is resumed, so the `abandoned` guard cannot be forgotten.
    static void resume_if_live(PermissionAwaitState& state) {
        if (state.abandoned || state.handle == nullptr) {
            return;
        }
        state.handle.resume();
    }

    PermissionResult await_resume() {
        if (!state_->result.has_value()) {
            return PermissionResult{};
        }
        return std::move(*state_->result);
    }

private:
    runtime::CompletionQueue* completions_ = nullptr;
    std::shared_ptr<PermissionAwaitState> state_;
};

} // namespace

runtime::Task<PermissionResult> PermissionService::request_body(runtime::TaskContext context,
                                                                PermissionService& self,
                                                                Permission permission,
                                                                PermissionRequestOptions options,
                                                                runtime::CancellationToken token) {
    (void)context;
    auto state = std::make_shared<PermissionAwaitState>();
    (void)self.submit(
        permission, std::move(options), std::move(token), [state](PermissionResult result) {
            state->result.emplace(std::move(result));
            if (state->handle == nullptr || state->resume_posted || state->abandoned) {
                return;
            }
            state->resume_posted = true;
            PermissionAwaiter::resume_if_live(*state);
        });
    // `submit` posts its callback, so it has not run yet; the awaiter handles both orders.
    PermissionResult result = co_await PermissionAwaiter(self.completion_queue(), std::move(state));
    co_return result;
}

} // namespace tessera::appfw
