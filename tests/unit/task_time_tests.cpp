// SPDX-License-Identifier: MIT

// The time-based combinator tests (issue #844): delay, timeout, and the eager-cancellation
// wakeup a parked parent leans on. Split from task_combinator_tests.cpp so each file stays
// under the source-size limit; the fixtures here are the ones these blocks drive.

#include "task_test_support.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/combinators.hpp"
#include "tessera/runtime/task_time.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <expected>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tessera::task_test;

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CancelReason;
using tessera::runtime::ManualClock;
using tessera::runtime::TaskAggregateError;
using tessera::runtime::TimedOut;
using tessera::runtime::TimerQueue;
using tessera::task_test::CompletionPriority;
using tessera::task_test::CompletionQueue;
using tessera::task_test::context;
using tessera::task_test::expect;
using tessera::task_test::pump;
using tessera::task_test::Task;
using tessera::task_test::TaskCancelled;
using tessera::task_test::TaskContext;

// True when the queue has nothing pending: no continuation, no completion. Every test block
// asserts this at the end, so a combinator can never leave work behind.
bool drained(CompletionQueue& queue) {
    return queue.empty() && queue.stats().pending_continuations == 0;
}

// Parks on a far deadline: the ManualClock never reaches it here, so only a cancellation can end
// the wait. The context needs the clock and the timer queue.
Task<void> park_on_delay(TaskContext ctx) {
    (void)co_await delay(ctx, 1000.0);
}

// A parent suspended on a parked child: the shape the eager-cancellation wakeup exists for. The
// parent's own context is unnamed, because the body only reads the child's.
Task<void> root_parked_child(TaskContext, TaskContext child_ctx) {
    co_await park_on_delay(child_ctx);
}

Task<int> number(TaskContext, int value) {
    co_return value;
}

Task<int> failing(TaskContext, const char* message) {
    throw std::runtime_error(message);
    // Not reached: a trailing co_return keeps the throw away from the coroutine epilogue, where
    // an exception out of the last statement trips an MSVC codegen edge (the exception then
    // unwinds the frame without the promise's exception capture seeing it).
    co_return 0;
}

Task<void> marking(TaskContext, bool& flag) {
    flag = true;
    co_return;
}

TaskContext time_context(CompletionQueue& queue, ManualClock& clock, TimerQueue& timers) {
    return TaskContext{&queue, CompletionPriority::Visible, {}, &clock, &timers};
}

Task<void> root_delay_deadline(TaskContext ctx) {
    (void)co_await delay(ctx, 2.5);
}

Task<void> root_delay_cancel(TaskContext ctx) {
    (void)co_await delay(ctx, 50.0);
}

Task<void> root_timeout_child_wins(TaskContext ctx, Task<int> child,
                                   CancellationSource& child_source, int& out) {
    const std::expected<int, TimedOut> result =
        co_await timeout(ctx, std::move(child), child_source, 5.0);
    if (result.has_value()) {
        out = *result;
    }
}

Task<void> root_timeout_deadline_wins(TaskContext ctx, Task<void> child,
                                      CancellationSource& child_source) {
    (void)co_await timeout(ctx, std::move(child), child_source, 1.0);
}

Task<void> root_timeout_child_fails(TaskContext ctx, Task<int> child,
                                    CancellationSource& child_source) {
    (void)co_await timeout(ctx, std::move(child), child_source, 5.0);
}

void rethrow_root_exception(const Task<void>& task) {
    if (task.exception() != nullptr) {
        std::rethrow_exception(task.exception());
    }
}

} // namespace

int main() {
    // --- delay: fires under ManualClock
    // -------------------------------------------------------------
    {
        ManualClock clock(10.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        auto task = root_delay_deadline(time_context(queue, clock, timers));
        (void)task.start();
        (void)pump(queue);
        if (!expect(timers.size() == 1, "the delay scheduled one timer") ||
            !expect(!task.done(), "not done before the deadline")) {
            return 11;
        }
        clock.advance(2.0);
        (void)timers.fire_due(clock.now_seconds());
        (void)pump(queue);
        if (!expect(!task.done(), "not done before the deadline passes")) {
            return 11;
        }
        clock.advance(0.5);
        (void)timers.fire_due(clock.now_seconds());
        (void)pump(queue);
        if (!expect(task.done() && !task.cancelled(), "done at the deadline") ||
            !expect(task.exception() == nullptr, "the delay resolved without an error") ||
            !expect(timers.size() == 0, "the one-shot removed itself") ||
            !expect(drained(queue), "queue drained")) {
            return 11;
        }
    }

    // --- delay: a cancelled token resumes promptly; the timer dies with the frame
    // ------------------
    {
        ManualClock clock(0.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        CancellationSource source;
        TaskContext ctx{&queue, CompletionPriority::Visible, source.token(), &clock, &timers};
        {
            auto task = root_delay_cancel(ctx);
            (void)task.start();
            (void)pump(queue);
            if (!expect(timers.size() == 1, "the delay had armed its timer")) {
                return 12;
            }
            source.cancel();
            (void)pump(queue);
            if (!expect(task.done() && task.cancelled(), "the delay task was cancelled") ||
                !expect(task.exception() != nullptr, "TaskCancelled stored") ||
                !expect(timers.size() == 1, "the timer outlives the finished-but-alive frame")) {
                return 12;
            }
            // Dropping the frame drops the timer with it: nothing can fire late.
        }
        if (!expect(timers.size() == 0, "the timer was removed with the frame") ||
            !expect(timers.fire_due(500.0) == 0, "nothing fired late") ||
            !expect(drained(queue), "queue drained")) {
            return 12;
        }
    }

    // --- timeout: the child wins
    // --------------------------------------------------------------------
    {
        ManualClock clock(0.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        CancellationSource child_source;
        int out = 0;
        auto task = root_timeout_child_wins(time_context(queue, clock, timers),
                                            number(context(queue), 7), child_source, out);
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done() && !task.cancelled(), "timeout done") ||
            !expect(out == 7, "the child's value came back") ||
            !expect(!child_source.cancelled(), "the child source stayed live") ||
            !expect(timers.size() == 0, "the deadline was disarmed after the child won") ||
            !expect(drained(queue), "queue drained")) {
            return 13;
        }
    }

    // --- timeout: the deadline wins; the child is cancelled with Timeout and drained
    // ----------------
    {
        ManualClock clock(0.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        CancellationSource child_source;
        TaskContext child{&queue, CompletionPriority::Visible, child_source.token(), &clock,
                          &timers};
        auto task = root_timeout_deadline_wins(time_context(queue, clock, timers),
                                               park_on_delay(child), child_source);
        (void)task.start();
        (void)pump(queue);
        if (!expect(timers.size() == 2, "the deadline and the child's park are scheduled")) {
            return 14;
        }
        clock.advance(1.0);
        (void)timers.fire_due(clock.now_seconds());
        (void)pump(queue);
        if (!expect(task.done(), "the timeout result fired") ||
            !expect(task.exception() == nullptr, "the timeout resolved as a value, not a throw") ||
            !expect(child_source.token().reason() == CancelReason::Timeout,
                    "the child was cancelled with Timeout") ||
            !expect(drained(queue), "the child drained before the caller resumed") ||
            !expect(timers.size() == 0, "no timer left armed")) {
            return 14;
        }
    }

    // --- timeout: a failing child propagates its exception, not an expected error
    // -------------------
    {
        ManualClock clock(0.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        CancellationSource child_source;
        auto task = root_timeout_child_fails(time_context(queue, clock, timers),
                                             failing(context(queue), "child"), child_source);
        (void)task.start();
        (void)pump(queue);
        bool caught = false;
        try {
            rethrow_root_exception(task);
        } catch (const std::runtime_error& error) {
            caught = error.what() == std::string("child");
        }
        if (!expect(caught, "the child's failure propagated") ||
            !expect(drained(queue), "drained")) {
            return 15;
        }
    }

    // --- eager cancellation: a parent suspended on a parked child wakes on cancel
    // -------------------
    {
        ManualClock clock(0.0);
        TimerQueue timers(8);
        CompletionQueue queue(16);
        CancellationSource parent_source;
        CancellationSource child_source;
        // The child parks on a delay far in the future, so only a cancellation can end it.
        TaskContext child{&queue, CompletionPriority::Visible, child_source.token(), &clock,
                          &timers};
        auto task = root_parked_child(
            TaskContext{&queue, CompletionPriority::Visible, parent_source.token()}, child);
        (void)task.start();
        (void)pump(queue);
        if (!expect(!task.done(), "the parent is suspended on the parked child") ||
            !expect(timers.size() == 1, "the child parked on one timer")) {
            return 16;
        }
        parent_source.cancel();
        (void)pump(queue);
        if (!expect(task.done() && task.cancelled(), "the parent woke promptly on cancel") ||
            !expect(!child_source.cancelled(), "the child was not touched (no fail-fast)")) {
            return 16;
        }
        // The parked child is still live; cancelling its own token ends it and drains the queue.
        child_source.cancel();
        (void)pump(queue);
        if (!expect(drained(queue), "everything drained after the child ended")) {
            return 16;
        }
    }

    // --- eager cancellation does not change tasks that have not started yet
    // -------------------------
    {
        CompletionQueue queue(16);
        CancellationSource source;
        bool ran = false;
        auto task = marking(TaskContext{&queue, CompletionPriority::Visible, source.token()}, ran);
        source.cancel();
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done() && task.cancelled() && !ran,
                    "a not-yet-started task still skips its body")) {
            return 17;
        }
    }
    std::cout << "task_time_tests: ok\n";
    return 0;
}
