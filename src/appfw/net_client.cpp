// SPDX-License-Identifier: MIT
//
// The public facade of `net` (docs/specs/networking-v1.md section 2; issue #197): the thin
// forwarding layer and the `send` coroutine. Everything it forwards to is in `net_client_impl.cpp`
// and `net_client_hop.cpp`.
//
// The `send` coroutine is the part that needs explaining. `runtime::Task`'s promise takes its
// `TaskContext` as the first constructor argument, and a member coroutine's implicit object
// parameter already occupies that position, so the body cannot be a member
// (docs/design/services.md section 1.1). It is therefore a context-first static member with the
// client passed after the context - the exact shape `services.md` section 1.1 documents for
// `Service::start`.
//
// It suspends on a small awaiter rather than on a `Task` of its own, because the result arrives
// through the client's callback path: `submit` takes the callback, the callback records the result
// in the awaiter's shared state, and the awaiter's resume is posted through the completion queue so
// the coroutine never continues inline on the resolving stack (the runtime's own rule for
// continuations).

#include "net_client_internal.hpp"
#include "tessera/appfw/net_client.hpp"

#include <coroutine>
#include <memory>
#include <optional>
#include <utility>

namespace tessera::appfw {

namespace {

// Shared between the coroutine frame and the client's callback. The callback can arrive before the
// coroutine reaches `await_suspend` (the record may already be resolved by the time the next resume
// happens), so "which came first" is carried in the state instead of being assumed.
struct NetSendState {
    std::optional<NetResult> result;
    std::coroutine_handle<> handle;
    bool resume_posted = false;
    // Set when the coroutine frame is destroyed before its request resolved - a caller that drops
    // the task it was awaiting. A posted or later resume then does nothing, instead of resuming a
    // destroyed frame.
    bool abandoned = false;
};

// The awaiter the `send` body suspends on.
class NetSendAwaiter {
public:
    NetSendAwaiter(std::shared_ptr<detail::NetClientImpl> impl, NetTicket ticket,
                   std::shared_ptr<NetSendState> state)
        : impl_(std::move(impl)),
          ticket_(ticket),
          state_(std::move(state)) {}

    // A destroyed frame must not be resumed by a completion that is already in the queue.
    ~NetSendAwaiter() {
        if (state_ != nullptr) {
            state_->abandoned = true;
        }
    }

    NetSendAwaiter(const NetSendAwaiter&) = delete;
    NetSendAwaiter& operator=(const NetSendAwaiter&) = delete;

    // False even when the result already arrived: `await_resume` has not run yet, and posting the
    // resume keeps the coroutine off the resolving stack.
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) const {
        state_->handle = handle;
        if (!state_->result.has_value() || state_->resume_posted) {
            return;
        }
        // The result beat the suspension: post the resume rather than returning false, so the
        // coroutine still continues from the queue instead of from this stack.
        state_->resume_posted = true;
        impl_->completions().post(runtime::CompletionPriority::Visible,
                                  [state = state_]() { resume_if_live(*state); });
    }

    // The one place a coroutine frame is resumed, so the `abandoned` guard cannot be forgotten.
    static void resume_if_live(NetSendState& state) {
        if (state.abandoned || state.handle == nullptr) {
            return;
        }
        state.handle.resume();
    }

    NetResult await_resume() {
        NetResult result = state_->result.has_value()
                               ? std::move(*state_->result)
                               : NetResult{std::unexpected(NetError{
                                     NetErrorKind::Other, "the request never resolved", 0})};
        // The record answered its purpose; freeing it here is what keeps `send` from filling the
        // client's record bound. A ticket already evicted is a no-op.
        impl_->release(ticket_);
        return result;
    }

private:
    std::shared_ptr<detail::NetClientImpl> impl_;
    NetTicket ticket_;
    std::shared_ptr<NetSendState> state_;
};

} // namespace

NetClient::NetClient(NetClientConfig config, NetTransport& transport,
                     runtime::IoTaskExecutor& executor, runtime::CompletionQueue& completions,
                     runtime::TimerQueue& timers, runtime::Clock& clock)
    : impl_(std::make_shared<detail::NetClientImpl>(std::move(config), transport, executor,
                                                    completions, timers, clock)) {}

NetClient::~NetClient() = default;

runtime::Task<NetResult> NetClient::send(NetRequest request, runtime::CancellationToken token) {
    // The task's own token is empty on purpose: cancellation is a *value* here (section 2's
    // `NetErrorKind::Cancelled`), while a task token would make the runtime throw `TaskCancelled`
    // at the `co_await`. The caller's token is what the request is cancelled with.
    runtime::TaskContext context;
    context.queue = &impl_->completions();
    context.priority = runtime::CompletionPriority::Visible;
    return send_body(context, *this, std::move(request), std::move(token));
}

runtime::Task<NetResult> NetClient::send_body(runtime::TaskContext, NetClient& self,
                                              NetRequest request,
                                              runtime::CancellationToken token) {
    auto state = std::make_shared<NetSendState>();
    const std::weak_ptr<detail::NetClientImpl> weak = self.impl_;
    auto submitted =
        self.impl_->submit(std::move(request), std::move(token), [weak, state](NetResult result) {
            state->result.emplace(std::move(result));
            if (state->handle == nullptr || state->resume_posted || state->abandoned) {
                return;
            }
            state->resume_posted = true;
            if (auto impl = weak.lock()) {
                impl->completions().post(runtime::CompletionPriority::Visible,
                                         [state]() { NetSendAwaiter::resume_if_live(*state); });
            }
        });
    if (!submitted.has_value()) {
        // A refused request is a value, not a throw: section 2's `bad_url`, `unsupported`, or the
        // client's own bound. It still arrives as a `NetResult`.
        co_return std::unexpected(submitted.error());
    }
    co_return co_await NetSendAwaiter{self.impl_, *submitted, std::move(state)};
}

std::expected<NetTicket, NetError>
NetClient::submit(NetRequest request, runtime::CancellationToken token, Callback on_done) {
    return impl_->submit(std::move(request), std::move(token), std::move(on_done));
}

bool NetClient::cancel(NetTicket ticket) {
    return impl_->cancel(ticket);
}

void NetClient::tick() {
    impl_->tick();
}

bool NetClient::release(NetTicket ticket) {
    return impl_->release(ticket);
}

NetRequestStatus NetClient::status_of(NetTicket ticket) const {
    return impl_->status_of(ticket);
}

const NetResponse* NetClient::response_of(NetTicket ticket) const {
    return impl_->response_of(ticket);
}

const NetError* NetClient::error_of(NetTicket ticket) const {
    return impl_->error_of(ticket);
}

std::size_t NetClient::in_flight() const noexcept {
    return impl_->in_flight();
}

std::size_t NetClient::records() const noexcept {
    return impl_->records();
}

const NetClientConfig& NetClient::config() const noexcept {
    return impl_->config();
}

NetDiagnostics NetClient::diagnostics() const {
    return impl_->diagnostics();
}

std::string_view NetClient::transport_name() const noexcept {
    return impl_->transport_name();
}

} // namespace tessera::appfw
