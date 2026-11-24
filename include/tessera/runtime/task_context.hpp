// SPDX-License-Identifier: MIT

// The vocabulary of a task's scheduling context: the cancellation exception a task observes and
// the `TaskContext` every task coroutine takes as its first parameter. Split out of task.hpp so
// the file stays under the source-size limit; the two types are what an awaitable needs to see.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/timer_queue.hpp"

namespace tessera::runtime {

// Thrown at the co_await boundary when the awaited task observed cancellation. Cancellation is
// cooperative: it is checked immediately before a task resumes, never asynchronously.
//
// `reason()` carries why the task's token was cancelled (issue #845), so an awaiter can tell a
// user abort from a timeout from a shutdown without a side channel. The default is `Requested`,
// which is what an unspecified `cancel()` means.
class TaskCancelled : public std::exception {
public:
    TaskCancelled() = default;
    explicit TaskCancelled(CancelReason reason) noexcept
        : reason_(reason) {}

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] CancelReason reason() const noexcept {
        return reason_;
    }

private:
    CancelReason reason_ = CancelReason::Requested;
};

// Scheduling context for one task. A task coroutine function takes it as its first parameter; the
// promise copies it, so the queue and the token must outlive the task.
//
// `clock` and `timers` are optional (null by default) seams for time-based awaiters such as
// `delay` and `timeout` (issue #844). A task that never awaits on time leaves them null; when they
// are set, both must outlive the task like the queue does. Deadlines are absolute on the injected
// clock, so a `ManualClock` makes time awaiters deterministic in tests.
struct TaskContext {
    CompletionQueue* queue = nullptr;
    CompletionPriority priority = CompletionPriority::Visible;
    CancellationToken token;
    Clock* clock = nullptr;
    TimerQueue* timers = nullptr;
};

} // namespace tessera::runtime
