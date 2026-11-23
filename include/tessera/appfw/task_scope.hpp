// SPDX-License-Identifier: MIT
//
// `TaskScope`: structured cancellation for work started by a service, a window or a component
// (ADR-050 section 5 item 1; AF2 of the roadmap).
//
// What the scope owns, and why each part exists:
//
//   * a `CancellationSource` whose token every child is created with, so `cancel()` is one atomic
//     flip and every child observes it at its next resume (`Task` checks the token before resuming
//     a frame, so a cancelled child stops without running another step);
//   * a bounded table of live children, so "no detached tasks" becomes structural: `spawn()` is the
//     only way to start a task here, a full scope refuses instead of growing, and `drained()` is a
//     question the shutdown sequencer can ask;
//   * a deadline-bounded `close()`, advanced by `tick()` rather than by waiting, so closing never
//     blocks the UI thread.
//
// Cancellation while a child is suspended is the hard case, and the design is deliberate. A child
// is wrapped in a token-free *driver* coroutine that awaits it, and the driver refers to the
// scope's bookkeeping through a **weak** reference. Two consequences follow:
//
//   * the coroutine frames are kept alive by the task graph itself (`TaskState` holds the frame;
//     the child holds a strong reference to its awaiter's state as the continuation), so
//     destroying the `TaskScope` while a child is suspended cancels the scope but never destroys a
//     frame another callback still holds a handle to;
//   * a driver that outlives the scope simply finds the bookkeeping gone and returns. Nothing
//     touches freed memory, and nothing is detached: the frame runs to completion on the
//     completion queue, which is the only place it can resume.
//
// A child that never finishes keeps its frame alive. That is a real cost, stated plainly rather
// than hidden: the driver frame and the child frame reference each other (the driver's awaiter
// holds the child's state; the child holds the driver's state as its continuation), so an abandoned
// pair is a reference cycle that is only broken when the child finally finishes. It is bounded by
// the scope's `capacity`, it happens only for a child that ignores its token, and it is visible:
// `live()` never reaches zero and `tick()` reports `TimedOut`.
//
// The one exception is a child parked on an awaitable that can never resume it: that awaitable
// declares so through `runtime::detail::declare_no_resume`, the child drops its reference to the
// driver, and the pair is freed when the slot is released (ADR-069). A child suspended on anything
// that can still resume it keeps the cycle.
//
// The alternative — destroying the frame — is not safe in general, because the awaitable the child
// is suspended on may hold a bare `coroutine_handle` and resume it later (that is exactly what the
// queue-hop fixture in the tests does). Destroying the frame would then resume freed memory. Since
// the scope cannot know which kind of await it is sitting on, it never destroys a suspended frame.
// This matches AF1's recorded trade-off for an abandoned service stop, which retains its bridge in
// `orphans` for the same reason.
//
// The driver's own `TaskContext` is token-free for the reason AF1 documents in
// `service_bridge.hpp`: a tokened driver would be skipped precisely when its child is cancelled,
// and the driver is what retires the slot.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace tessera::appfw {

struct TaskScopeConfig {
    // Bound on live children. A `spawn()` past it is refused instead of growing the table. Zero is
    // treated as one so a scope can never be unable to hold anything by accident.
    std::size_t capacity = 32;
    runtime::CompletionPriority priority = runtime::CompletionPriority::Visible;
};

// What one `close()` attempt is doing. `Closing` means "cancelled, children still winding down";
// `TimedOut` means the deadline expired with children still live. `Closed` is the only state that
// promises the scope holds no child; a scope may move from `TimedOut` to `Closed` if the children
// it had given up on finish after all, so a host that keeps ticking eventually sees the truth.
enum class TaskScopeCloseState : std::uint8_t {
    Open,
    Closing,
    Closed,
    TimedOut,
};

namespace detail {

// Shared with the `TaskScope` handle only. Drivers hold it weakly, which is what makes destroying
// the handle while children are suspended safe.
struct TaskScopeState {
    struct Slot {
        runtime::Task<void> task;
        std::uint32_t generation = 0;
        bool live = false;
    };

    TaskScopeState(runtime::Clock& clock_source, runtime::CompletionQueue& queue_source,
                   TaskScopeConfig scope_config);

    [[nodiscard]] runtime::TaskContext child_context() const;
    // Deliberately token-free: see the header comment.
    [[nodiscard]] runtime::TaskContext driver_context() const;

    // Reserves a slot. False when the scope is closed, cancelled or full.
    bool reserve(std::size_t& index, std::uint32_t& generation);
    // Stores and starts the driver. On refusal the slot is released and nothing is left running.
    bool adopt(std::size_t index, std::uint32_t generation, runtime::Task<void> task);
    // Marks the slot free. It does **not** drop the finished handle: `retire` runs inside the
    // driver's own frame, and destroying the frame that is executing is undefined behaviour. The
    // dead handle is released when the slot is reused or the scope is destroyed, both of which
    // happen outside any child's body.
    void retire(std::size_t index, std::uint32_t generation) noexcept;
    void record_failure() noexcept;
    // Records that a child observed the scope's token and returned early.
    void record_cancelled_child() noexcept;

    runtime::Clock* clock = nullptr;
    runtime::CompletionQueue* completions = nullptr;
    TaskScopeConfig config;
    // The scope token. With a parent it is a linked source (issue #845), so a parent cancel
    // arrives with reason `Parent` and the link is unregistered when this scope is destroyed.
    runtime::CancellationSource cancel;
    std::unique_ptr<Slot[]> slots;
    std::size_t capacity = 1;
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t spawned = 0;
    std::size_t finished = 0;
    std::size_t failed = 0;
    // Children that exited by observing the scope's token. Separate from `failed` because
    // cancellation is the normal shutdown path, not a defect.
    std::size_t cancelled_children = 0;
    std::size_t refused = 0;
    bool closing = false;
    // Latched when a close deadline expired with children still live. It keeps `tick()` reporting
    // `TimedOut` instead of falling back to `Closing`; if the children do retire later the scope
    // still reports `Closed`, because that is the truth and a host should stop waiting then.
    bool timed_out = false;
    double deadline_seconds = 0.0;
};

// The driver: awaits `task`, turns an escaped exception into a counter, and always retires its
// slot. `context` is an unnamed leading parameter because the promise consumes it, not the body.
template<typename T>
runtime::Task<void> drive_child(runtime::TaskContext, std::weak_ptr<TaskScopeState> weak_state,
                                std::size_t index, std::uint32_t generation,
                                runtime::Task<T> task) {
    bool cancelled = false;
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
        } else {
            (void)co_await std::move(task);
        }
    } catch (const runtime::TaskCancelled&) {
        // The runtime skipped a resume because the scope's token was flipped: the normal shutdown
        // path, distinguished from a child that actually failed.
        cancelled = true;
    } catch (...) {
        // A child's failure is a value for its owner, never an exception into the frame loop.
        if (const std::shared_ptr<TaskScopeState> state = weak_state.lock()) {
            state->record_failure();
        }
    }
    if (const std::shared_ptr<TaskScopeState> state = weak_state.lock()) {
        if (cancelled) {
            state->record_cancelled_child();
        }
        state->retire(index, generation);
    }
    co_return;
}

} // namespace detail

class TaskScope {
public:
    // `clock` bounds `close()`; `completions` is where every child's resume runs. Both are borrowed
    // and must outlive this object and its children. `parent` is optional: an already-cancelled
    // parent cancels this scope immediately, otherwise this scope is cancelled when the parent is.
    // That is how scopes nest (app > service > window > component) without a child owning a parent.
    TaskScope(runtime::Clock& clock, runtime::CompletionQueue& completions,
              TaskScopeConfig config = {}, runtime::CancellationToken parent = {});

    // Cancels and releases this handle. Children that are still suspended keep running to their own
    // completion (see the header comment) and their frames are freed when they finish.
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    // The context a child task must be created with; it carries this scope's cancellation token.
    // A child coroutine takes it as its first parameter, and the token is captured when the task is
    // created - `spawn()` cannot retrofit one, so a task created with some other context is not
    // cancellable by this scope even though `spawn()` still starts and accounts for it.
    [[nodiscard]] runtime::TaskContext context() const;

    // Starts `task` under the scope. False when the scope is closed/cancelled, full, or when the
    // completion queue refused the first resume. Nothing is detached and nothing runs inline: the
    // first resume is posted to the queue like any other completion.
    template<typename T> bool spawn(runtime::Task<T> task) {
        std::size_t index = 0;
        std::uint32_t generation = 0;
        if (!state_->reserve(index, generation)) {
            return false;
        }
        return state_->adopt(index, generation,
                             detail::drive_child(state_->driver_context(),
                                                 std::weak_ptr<detail::TaskScopeState>(state_),
                                                 index, generation, std::move(task)));
    }

    // Cooperative and idempotent: flips the token, which every child observes at its next resume.
    // It never runs, destroys or waits for a child.
    void cancel();

    // Requests a bounded shutdown: cancels, records an absolute deadline on the scope's clock and
    // returns immediately. `tick()` reports progress, so nothing here waits on the UI thread.
    void close(double deadline_seconds);

    // Advances the close sequence and reports it. Never blocks and never runs a child coroutine
    // (the completion queue does that).
    TaskScopeCloseState tick();
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] bool closing() const noexcept;
    // True when no child is live: the question the shutdown sequencer asks after `close()`.
    [[nodiscard]] bool drained() const noexcept;

    [[nodiscard]] std::size_t live() const noexcept;
    [[nodiscard]] std::size_t peak() const noexcept;
    [[nodiscard]] std::size_t spawned() const noexcept;
    [[nodiscard]] std::size_t finished() const noexcept;
    [[nodiscard]] std::size_t failed() const noexcept;
    // Children that exited through `TaskCancelled` rather than a normal return.
    [[nodiscard]] std::size_t cancelled_children() const noexcept;
    // Spawn attempts refused (closed, cancelled or at capacity).
    [[nodiscard]] std::size_t refused() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    // Shared, not owned outright, so a handler that outlives this handle cannot dangle. The drivers
    // hold it weakly.
    std::shared_ptr<detail::TaskScopeState> state_;
};

} // namespace tessera::appfw
