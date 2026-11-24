// SPDX-License-Identifier: MIT
//
// Issue #844: the task combinators (`when_all`, `when_all_range`, `when_any`, `when_any_value`)
// and the time helpers (`delay`, `timeout`), plus the eager-cancellation wakeup they lean on.
// Deterministic throughout: a `ManualClock` drives the timers and the queue is pumped by hand.
// Roots wrap the combinator call so the awaited result lands in a reference the test can read.

#include "task_test_support.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/combinators.hpp"
#include "tessera/runtime/task_time.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <expected>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CancelReason;
using tessera::runtime::ManualClock;
using tessera::runtime::TaskAggregateError;
using tessera::runtime::TaskTimeError;
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

// --- coroutine fixtures ---------------------------------------------------------------------

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

// Two await rounds, so a `when_any` race against an immediate child loses deterministically.
Task<int> slow_double(TaskContext ctx, int value, bool& done) {
    const int first = co_await number(ctx, value);
    const int second = co_await number(ctx, first);
    done = true;
    co_return second;
}

// Parks on a far deadline: the ManualClock never reaches it here, so only a cancellation can end
// the wait. The context needs the clock and the timer queue.
Task<void> park_on_delay(TaskContext ctx) {
    (void)co_await delay(ctx, 1000.0);
}

TaskContext time_context(CompletionQueue& queue, ManualClock& clock, TimerQueue& timers) {
    return TaskContext{&queue, CompletionPriority::Visible, {}, &clock, &timers};
}

// The "no task left live" assertion every test block ends with.
bool drained(const CompletionQueue& queue) {
    return queue.empty() && queue.stats().pending_continuations == 0;
}

// Counts the non-null failure slots of an aggregate.
std::size_t count_failures(const TaskAggregateError& error) {
    std::size_t failed = 0;
    for (const std::exception_ptr& failure : error.failures()) {
        if (failure != nullptr) {
            ++failed;
        }
    }
    return failed;
}

// --- roots -----------------------------------------------------------------------------------

Task<void> root_all_values(TaskContext ctx, int& first, int& second, bool& flag) {
    const auto [a, b, unit] =
        co_await when_all(ctx, number(ctx, 1), number(ctx, 2), marking(ctx, flag));
    (void)unit;
    first = a;
    second = b;
}

Task<void> root_all_failure(TaskContext ctx, bool& late_ran) {
    (void)co_await when_all(ctx, number(ctx, 1), failing(ctx, "mid"), number(ctx, 3),
                            marking(ctx, late_ran));
    // Not reached when a child failed: a trailing statement keeps the throwing await from being
    // the last thing in the body, which trips an MSVC coroutine codegen edge (the exception then
    // never reaches the promise's exception capture and vanishes at the frame epilogue).
    late_ran = late_ran;
}

Task<void> root_all_cancelled_first(TaskContext ctx, TaskContext cancelled_child, bool& sibling) {
    (void)co_await when_all(ctx, marking(cancelled_child, sibling), number(ctx, 5));
    sibling = sibling; // trailing statement: see root_all_failure
}

Task<void> root_all_cancelled_last(TaskContext ctx, TaskContext cancelled_child, bool& sibling) {
    (void)co_await when_all(ctx, failing(ctx, "first"), marking(cancelled_child, sibling));
    sibling = sibling; // trailing statement: see root_all_failure
}

Task<void> root_all_range(TaskContext ctx, std::span<Task<int>> children, std::vector<int>& out) {
    out = co_await when_all_range(ctx, children);
}

Task<void> root_all_range_empty(TaskContext ctx, std::vector<int>& out) {
    std::vector<Task<int>> children;
    out = co_await when_all_range(ctx, std::span<Task<int>>(children));
}

Task<void> root_any_index(TaskContext ctx, Task<int> loser, Task<int> winner, bool& loser_done,
                          std::size_t& index) {
    index = co_await when_any(ctx, std::move(loser), std::move(winner));
    (void)loser_done;
}

Task<void> root_any_failing_winner(TaskContext ctx, Task<int> loser, bool& loser_done) {
    (void)co_await when_any(ctx, std::move(loser), failing(ctx, "won"));
}

Task<void> root_any_value_winner(TaskContext ctx, Task<int> loser, Task<int> winner,
                                 bool& loser_done, int& out) {
    out = co_await when_any_value(ctx, std::move(loser), std::move(winner));
    (void)loser_done;
}

Task<void> root_any_value_failing(TaskContext ctx, Task<int> loser, Task<int> winner) {
    (void)co_await when_any_value(ctx, std::move(loser), std::move(winner));
    // Trailing statement: a throwing await as the body's last act trips the MSVC codegen edge
    // documented in `combinators.hpp` (the exception never reaches the promise's capture).
    co_return;
}

Task<void> root_any_range_empty(TaskContext ctx) {
    std::vector<Task<int>> children;
    (void)co_await when_any_range(ctx, std::span<Task<int>>(children));
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

// A parent suspended on a parked child: the shape the eager-cancellation wakeup exists for. The
// parent's own context is taken unnamed, because the body only reads the child's.
Task<void> root_parked_child(TaskContext, TaskContext child_ctx) {
    co_await park_on_delay(child_ctx);
}

// Rethrows the root's stored exception when there is one, so `catch` blocks can inspect it.
void rethrow_root_exception(const Task<void>& task) {
    if (task.exception() != nullptr) {
        std::rethrow_exception(task.exception());
    }
}

} // namespace

int main() {
    // --- when_all: values in argument order, void children become monostate ----------------------
    {
        CompletionQueue queue(16);
        int first = 0;
        int second = 0;
        bool flag = false;
        auto task = root_all_values(context(queue), first, second, flag);
        (void)task.start();
        std::size_t pumped = 0;
        while (!queue.empty() && pumped < 32) {
            pumped += queue.run(1);
        }
        if (!expect(task.done() && !task.cancelled(), "when_all done") ||
            !expect(first == 1 && second == 2, "values in argument order") ||
            !expect(flag, "void child ran") || !expect(drained(queue), "queue drained")) {
            return 1;
        }
    }

    // --- when_all: a failing child does not stop siblings; the aggregate keeps every failure
    // ------
    {
        CompletionQueue queue(16);
        bool late_ran = false;
        auto task = root_all_failure(context(queue), late_ran);
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done(), "when_all done despite the failure")) {
            return 2;
        }
        bool caught_aggregate = false;
        try {
            rethrow_root_exception(task);
        } catch (const TaskAggregateError& error) {
            caught_aggregate = true;
            const std::vector<std::exception_ptr>& failures = error.failures();
            if (!expect(failures.size() == 4, "one failure slot per child") ||
                !expect(failures[0] == nullptr && failures[2] == nullptr && failures[3] == nullptr,
                        "successful children have null slots") ||
                !expect(error.first() == failures[1], "first() is the first failure in order") ||
                !expect(count_failures(error) == 1, "exactly one failure recorded")) {
                return 2;
            }
            try {
                std::rethrow_exception(error.first());
            } catch (const std::runtime_error& thrown) {
                if (!expect(thrown.what() == std::string("mid"),
                            "the original exception survives")) {
                    return 2;
                }
            } catch (...) {
                return 2;
            }
            if (!expect(std::string(error.what()).find("1 of 4") != std::string::npos,
                        "what() names the failure count")) {
                return 2;
            }
        } catch (...) {
            return 2;
        }
        if (!expect(caught_aggregate, "the aggregate is thrown") ||
            !expect(late_ran, "siblings ran to completion after the failure") ||
            !expect(drained(queue), "queue drained")) {
            return 2;
        }
    }

    // --- when_all: a cancellation first in argument order flows through as TaskCancelled
    // ----------
    {
        CompletionQueue queue(16);
        CancellationSource dead;
        dead.cancel();
        bool sibling = false;
        TaskContext child{&queue, CompletionPriority::Visible, dead.token()};
        auto task = root_all_cancelled_first(context(queue), child, sibling);
        (void)task.start();
        (void)pump(queue);
        bool caught_cancel = false;
        CancelReason reason = CancelReason::Requested;
        try {
            rethrow_root_exception(task);
        } catch (const TaskCancelled& cancelled) {
            caught_cancel = true;
            reason = cancelled.reason();
        } catch (const TaskAggregateError&) {
        }
        if (!expect(caught_cancel, "TaskCancelled flows through when_all") ||
            !expect(reason == CancelReason::Requested, "the reason survives") ||
            !expect(drained(queue), "queue drained")) {
            return 3;
        }
    }

    // --- when_all: a non-cancellation first failure wins over a later cancellation
    // ----------------
    {
        CompletionQueue queue(16);
        CancellationSource dead;
        dead.cancel();
        TaskContext child{&queue, CompletionPriority::Visible, dead.token()};
        bool sibling = false;
        auto task = root_all_cancelled_last(context(queue), child, sibling);
        (void)task.start();
        (void)pump(queue);
        bool caught_aggregate = false;
        bool caught_cancel = false;
        try {
            rethrow_root_exception(task);
        } catch (const TaskAggregateError& error) {
            caught_aggregate = true;
            if (!expect(count_failures(error) == 2, "both failures recorded")) {
                return 4;
            }
            try {
                std::rethrow_exception(error.failures()[1]);
            } catch (const TaskCancelled&) {
                if (!expect(true, "the later cancellation is in the aggregate")) {
                    return 4;
                }
            } catch (...) {
                return 4;
            }
        } catch (const TaskCancelled&) {
            caught_cancel = true;
        }
        if (!expect(caught_aggregate && !caught_cancel,
                    "the aggregate wins when the first failure is not a cancel") ||
            !expect(drained(queue), "queue drained")) {
            return 4;
        }
    }

    // --- when_all_range: values in span order, failures per index; the empty span is trivial
    // ------
    {
        CompletionQueue queue(16);
        std::vector<Task<int>> children;
        children.push_back(number(context(queue), 7));
        children.push_back(failing(context(queue), "second"));
        children.push_back(number(context(queue), 9));
        std::vector<int> values;
        auto task = root_all_range(context(queue), std::span<Task<int>>(children), values);
        (void)task.start();
        (void)pump(queue);
        bool caught = false;
        try {
            rethrow_root_exception(task);
        } catch (const TaskAggregateError& error) {
            caught = true;
            if (!expect(error.failures().size() == 3 && error.first() == error.failures()[1],
                        "range failures are indexed by span position")) {
                return 5;
            }
        }
        if (!expect(caught, "range failure surfaces as the aggregate") ||
            !expect(drained(queue), "queue drained")) {
            return 5;
        }
    }
    {
        CompletionQueue queue(16);
        std::vector<int> values = {1};
        auto task = root_all_range_empty(context(queue), values);
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done() && !task.cancelled(), "empty range done") ||
            !expect(values.empty(), "empty range yields an empty vector") ||
            !expect(drained(queue), "queue drained")) {
            return 6;
        }
    }

    // --- when_any: the first finisher wins; losers keep running
    // ------------------------------------
    {
        CompletionQueue queue(16);
        bool loser_done = false;
        std::size_t index = 99;
        TaskContext ctx = context(queue);
        auto loser = slow_double(ctx, 3, loser_done);
        auto winner = number(ctx, 8);
        auto task = root_any_index(ctx, std::move(loser), std::move(winner), loser_done, index);
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done(), "when_any done") ||
            !expect(index == 1, "the faster child won by index")) {
            return 7;
        }
        // The loser was never cancelled: pump the rest of the graph and watch it finish.
        (void)pump(queue);
        if (!expect(loser_done, "the abandoned child still ran to completion") ||
            !expect(drained(queue), "queue drained")) {
            return 7;
        }
    }

    // --- when_any: a failing winner finishes the race and rethrows its exception
    // -------------------
    {
        CompletionQueue queue(16);
        bool loser_done = false;
        TaskContext ctx = context(queue);
        auto loser = slow_double(ctx, 3, loser_done);
        auto task = root_any_failing_winner(ctx, std::move(loser), loser_done);
        (void)task.start();
        (void)pump(queue);
        bool caught = false;
        try {
            rethrow_root_exception(task);
        } catch (const std::runtime_error& error) {
            caught = error.what() == std::string("won");
        }
        if (!expect(caught, "the winner's exception is rethrown") ||
            !expect(loser_done, "the loser ran") || !expect(drained(queue), "queue drained")) {
            return 8;
        }
    }

    // --- when_any_value: the winner's value, or its exception
    // --------------------------------------
    {
        CompletionQueue queue(16);
        bool loser_done = false;
        int out = 0;
        TaskContext ctx = context(queue);
        auto loser = slow_double(ctx, 4, loser_done);
        auto task = root_any_value_winner(ctx, std::move(loser), number(ctx, 41), loser_done, out);
        (void)task.start();
        (void)pump(queue);
        if (!expect(task.done() && out == 41, "the winner's value came back") ||
            !expect(loser_done, "the loser still completed") ||
            !expect(drained(queue), "queue drained")) {
            return 9;
        }
    }
    {
        CompletionQueue queue(16);
        TaskContext ctx = context(queue);
        bool loser_done = false;
        auto loser = slow_double(ctx, 2, loser_done);
        auto task = root_any_value_failing(ctx, std::move(loser), failing(ctx, "won"));
        (void)task.start();
        (void)pump(queue);
        bool caught = false;
        try {
            rethrow_root_exception(task);
        } catch (const std::runtime_error& error) {
            caught = error.what() == std::string("won");
        }
        if (!expect(caught, "a failing winner rethrows through when_any_value") ||
            !expect(loser_done, "the loser still completed") ||
            !expect(drained(queue), "queue drained")) {
            return 9;
        }
    }

    // --- when_any_range: an empty span is rejected at the co_await
    // ---------------------------------
    {
        CompletionQueue queue(16);
        auto task = root_any_range_empty(context(queue));
        (void)task.start();
        (void)pump(queue);
        bool caught = false;
        try {
            rethrow_root_exception(task);
        } catch (const std::invalid_argument&) {
            caught = true;
        }
        if (!expect(caught, "an empty when_any_range is rejected") ||
            !expect(drained(queue), "queue drained")) {
            return 10;
        }
    }

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

    std::cout << "task_combinators: ok\n";
    return 0;
}
