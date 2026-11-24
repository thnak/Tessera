// SPDX-License-Identifier: MIT
//
// Time-based task helpers over `TimerQueue` (issue #844): `delay` and `timeout`. Determinism comes
// from the context's `Clock`: deadlines are absolute on that clock, so a `ManualClock` fires them
// exactly. The `TimerQueue` has no internal locking, so a timer is only ever touched from the
// drain thread: a cancellation callback (which runs on the cancelling thread) must not cancel a
// timer directly; it posts the task's resume instead, and the queued resume does the cleanup.

#pragma once

#include "tessera/runtime/task.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace tessera::runtime {

// Why `delay` refused to run. Returned through `std::expected` because these are recoverable
// input errors, not failures of the awaited work. `timeout` reports the same conditions as
// `std::invalid_argument`, because its result type has no room for a second error kind.
enum class TaskTimeError : std::uint8_t {
    // The context carried no `TimerQueue` or no `Clock`, so no deadline can be scheduled.
    NoTimeSeams = 1,
    // The requested duration was negative or non-finite.
    InvalidDuration,
};

// Marker carried in `timeout`'s `std::expected` when the deadline elapsed. A small named type
// rather than a bare bool, so a caller reads `result.error() == TimedOut{}` and a future reason
// (such as the winning cancel reason) can extend it without changing the shape.
struct TimedOut {
    [[nodiscard]] friend constexpr bool operator==(TimedOut, TimedOut) = default;
};

namespace detail {

// One armed deadline timer plus the resume path of the task suspended on it. Shared between the
// timer callback and the awaiting task so that whichever side ends the wait first disarms the
// other: the timer posts the resume, and the resumed task (or the destructor, when the frame dies
// first) cancels the timer. Everything here runs on the drain thread.
class TimerWait final {
public:
    TimerWait() = default;
    TimerWait(const TimerWait&) = delete;
    TimerWait& operator=(const TimerWait&) = delete;

    ~TimerWait() {
        // Destruction after an unobserved deadline drops the timer; a stale id is harmless.
        if (timers != nullptr && id.has_value()) {
            timers->cancel(*id);
        }
    }

    void set_state(TaskStateBase* state) {
        state_ = state;
    }

    // Runs from the timer callback: posts the suspended task's resume. The fired one-shot removes
    // itself, so no `cancel` is needed here.
    void fire() {
        if (state_ != nullptr) {
            state_->post_resume();
        }
    }

    // True for the first arrival, which owns the timer cleanup. The state pointer is dropped too,
    // so a later fire cannot post into a task that already moved on.
    bool claim_resume() {
        if (claimed) {
            return false;
        }
        claimed = true;
        state_ = nullptr;
        return true;
    }

    void disarm_timer() {
        if (timers != nullptr && id.has_value()) {
            timers->cancel(*id);
            id.reset();
        }
    }

    TimerQueue* timers = nullptr;
    std::optional<TimerId> id;

private:
    TaskStateBase* state_ = nullptr;
    bool claimed = false;
};

// The awaiter `delay` suspends on. `await_suspend` arms the eager-cancellation hook and schedules
// the one-shot timer; the first of {timer fire, cancel post} resumes the task, and the resumed
// body (on the drain thread) does the timer cleanup.
class DelayAwaiter {
public:
    explicit DelayAwaiter(Clock& clock, TimerQueue& timers, double seconds)
        : clock_(clock),
          seconds_(seconds) {
        wait_ = std::make_shared<TimerWait>();
        wait_->timers = &timers;
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template<typename ParentPromise>
    bool await_suspend(std::coroutine_handle<ParentPromise> parent) {
        std::shared_ptr<TaskStateBase> state = parent.promise().self_.lock();
        if (state == nullptr) {
            throw std::logic_error("task awaited outside a Tessera task coroutine");
        }
        // Arm the wakeup BEFORE scheduling, so a cancel landing between the two still posts a
        // resume (the registration runs inline on an already-cancelled token). The hook only
        // posts; the timer cleanup happens in the queued resume below.
        state->arm_cancel_wakeup();
        wait_->set_state(state.get());
        const std::weak_ptr<TimerWait> weak = wait_;
        const std::optional<TimerId> id =
            wait_->timers->schedule_one_shot(clock_.now_seconds() + seconds_, [weak] {
                if (const std::shared_ptr<TimerWait> locked = weak.lock()) {
                    locked->fire();
                }
            });
        if (!id) {
            throw std::runtime_error("timer queue rejected the delay deadline");
        }
        wait_->id = *id;
        return true;
    }

    void await_resume() {
        // First arrival wins the cleanup: on the timer path the deadline is met, on the cancel
        // path the token is cancelled and the queued resume marks the task cancelled. A second
        // post (cancel after fire, or the reverse) is absorbed by the coalescing resume slot.
        if (wait_->claim_resume()) {
            wait_->disarm_timer();
        }
    }

private:
    std::shared_ptr<TimerWait> wait_;
    Clock& clock_;
    double seconds_;
};

// The race behind `timeout`: the child driver records the outcome and is the ONLY side that wakes
// the parent, which is what makes the drain guarantee hold - the deadline timer cancels the child
// source and then lets the child's own unwinding (through the driver) do the waking. Whether the
// deadline or the child won is read from the child source's `deadline_fired` flag, which its
// `cancel_after` timer sets before cancelling.
template<typename T> class TimeoutRace final {
public:
    TimeoutRace() = default;
    TimeoutRace(const TimeoutRace&) = delete;
    TimeoutRace& operator=(const TimeoutRace&) = delete;

    std::shared_ptr<TaskStateBase> parent;
    bool child_finished = false;
    std::exception_ptr child_failure;
    std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value;

    void wake_parent() {
        if (parent != nullptr) {
            parent->post_resume();
            parent.reset();
        }
    }

    // Called by the driver after the child finished (value, exception or cancellation): the
    // parent may resume now, because the drain it must wait for is over.
    void child_done() {
        child_finished = true;
        wake_parent();
    }
};

// Runs the timed-out child to its end so nothing is left live. The driver context carries NO
// token on purpose: the driver must never be cancelled, because its whole job is to observe the
// child to completion - a cancelled driver would never record the outcome and `timeout` would
// hang instead of draining.
template<typename T>
Task<void> drive_timeout_child(TaskContext, std::shared_ptr<TimeoutRace<T>> race,
                               Task<T> child) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(child);
        } else {
            race->value.emplace(co_await std::move(child));
        }
    } catch (...) {
        race->child_failure = std::current_exception();
    }
    race->child_done();
}

// The awaiter `timeout` suspends on. It arms the parent's eager-cancellation hook, schedules the
// deadline timer on the caller's behalf, and starts the child under its driver. The timer's
// lifetime is the armed `CancellationSource` deadline, which `cancel_after` already owns (removing
// the timer when the source dies), so the awaiter holds no timer of its own - one deadline, one
// timer, and no double-arm race.
template<typename T> class TimeoutAwaiter {
public:
    TimeoutAwaiter(TaskContext ctx, Task<T> task, CancellationSource& child_source, double seconds)
        : ctx_(std::move(ctx)),
          task_(std::move(task)),
          child_source_(child_source),
          seconds_(seconds) {}

    ~TimeoutAwaiter() {
        // The frame died before the race resolved (abandoned/caller-destroyed): drop the armed
        // deadline so it cannot cancel a later operation on the source at a stale instant.
        if (race_ != nullptr) {
            child_source_.cancel_deadline();
        }
    }

    TimeoutAwaiter(const TimeoutAwaiter&) = delete;
    TimeoutAwaiter& operator=(const TimeoutAwaiter&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template<typename ParentPromise>
    bool await_suspend(std::coroutine_handle<ParentPromise> parent) {
        std::shared_ptr<TaskStateBase> state = parent.promise().self_.lock();
        if (state == nullptr) {
            throw std::logic_error("task awaited outside a Tessera task coroutine");
        }
        // Caller cancellation wakes this frame promptly; the queued resume then marks the task
        // cancelled (TaskCancelled at the caller). The child is untouched: its lifetime is owned
        // by `child_source`, not by this frame's token.
        state->arm_cancel_wakeup();
        race_->parent = std::move(state);

        // The deadline arms the child source with `CancelReason::Timeout` and deliberately does
        // NOT wake the parent: the child's drain (through its driver) is the wake, so the caller
        // never resumes before the abandoned child finished. The timer lives inside the source's
        // `cancel_after` registration and dies with it.
        if (!child_source_.cancel_after(*ctx_.clock, *ctx_.timers, seconds_)) {
            // The source was already cancelled or the timer queue refused the deadline: no
            // timeout is possible. Thrown (not returned through `expected`), because it is a
            // wiring error at the call site; the promise stores it and it surfaces at the
            // caller's `co_await`, like every combinator input error.
            throw std::invalid_argument(
                "timeout: the child source is already cancelled or the timer queue refused the "
                "deadline");
        }
        Task<void> driver = drive_timeout_child(TaskContext{ctx_.queue, ctx_.priority, {}}, race_,
                                                std::move(task_));
        (void)driver.start();
        return true;
    }

    std::expected<T, TimedOut> await_resume() {
        race_->parent.reset();
        // The deadline wins when its timer fired and the source was cancelled with Timeout - the
        // wake arrived through the driver after the child drained, so the drain guarantee holds.
        if (child_source_.deadline_fired()) {
            return std::unexpected(TimedOut{});
        }
        if (race_->child_finished) {
            if (race_->child_failure != nullptr) {
                std::rethrow_exception(race_->child_failure);
            }
            // The child won the race: the armed deadline is moot, and leaving it armed would
            // cancel a later operation on this source at a stale instant.
            child_source_.cancel_deadline();
            if constexpr (std::is_void_v<T>) {
                return {};
            } else {
                return std::expected<T, TimedOut>(std::move(*race_->value));
            }
        }
        // Unreachable: every wake comes from `child_done` or from a cancellation (which kills the
        // frame before `await_resume`). Kept total so the function has no accidental fall-through.
        return std::unexpected(TimedOut{});
    }

private:
    TaskContext ctx_;
    Task<T> task_;
    CancellationSource& child_source_;
    double seconds_;
    std::shared_ptr<TimeoutRace<T>> race_ = std::make_shared<TimeoutRace<T>>();
};

} // namespace detail

// Resolves after `seconds` on the context's clock, or throws `TaskCancelled` promptly when the
// task's token is cancelled while waiting (the timer is removed; nothing fires late). The clock
// and the timer queue come from the task's `TaskContext`; without them, or for an invalid
// duration, the task resolves to `std::unexpected(TaskTimeError::...)` instead of throwing.
inline Task<std::expected<void, TaskTimeError>> delay(TaskContext ctx, double seconds) {
    if (ctx.clock == nullptr || ctx.timers == nullptr) {
        co_return std::unexpected(TaskTimeError::NoTimeSeams);
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        co_return std::unexpected(TaskTimeError::InvalidDuration);
    }
    // The catch-and-rethrow keeps the frame's unwind path well-formed: an exception out of the
    // last await would trip the MSVC codegen edge where `unhandled_exception` is never called
    // (see `combinators.hpp`).
    std::exception_ptr captured;
    try {
        co_await detail::DelayAwaiter{*ctx.clock, *ctx.timers, seconds};
    } catch (...) {
        captured = std::current_exception();
    }
    if (captured != nullptr) {
        std::rethrow_exception(std::move(captured));
    }
    co_return {};
}

// Races `task` against a deadline. The child wins: its value comes back and `child_source` stays
// live (the armed deadline is disarmed). The deadline wins: the child is cancelled with
// `CancelReason::Timeout`, the helper waits for the child to observe the cancellation and finish
// (the drain), and only then returns `std::unexpected(TimedOut{})`, so no task is left live; the
// timeout result therefore fires after the drain. A child that fails with a real exception
// propagates that exception, because a failure is not a timeout. The caller cancelling the
// timeout task itself surfaces as `TaskCancelled` and does not cancel the child: the child's
// lifetime is owned by `child_source`. A child that can never observe the cancellation (it never
// resumes) blocks the drain, and with it the timeout result. A context without clock/timer seams,
// an invalid duration, or a source that cannot arm its deadline throws `std::invalid_argument` -
// a wiring error, and the issue's result type has no room for a second error kind.
template<typename T>
Task<std::expected<T, TimedOut>> timeout(TaskContext ctx, Task<T> task,
                                         CancellationSource& child_source, double seconds) {
    if (ctx.clock == nullptr || ctx.timers == nullptr) {
        throw std::invalid_argument("timeout: the task context has no clock or timer queue");
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        throw std::invalid_argument("timeout: the duration must be finite and non-negative");
    }
    // Catch-and-rethrow keeps the unwind path well-formed; see `combinators.hpp`.
    std::exception_ptr captured;
    try {
        co_return co_await detail::TimeoutAwaiter<T>(std::move(ctx), std::move(task), child_source,
                                                     seconds);
    } catch (...) {
        captured = std::current_exception();
    }
    std::rethrow_exception(std::move(captured));
}

} // namespace tessera::runtime
