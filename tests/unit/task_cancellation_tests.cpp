// SPDX-License-Identifier: MIT
//
// #843 (a task resume is never dropped), #845 (cancellation reasons, linked sources, deadlines) and
// #842 (one wake per empty -> non-empty edge). The base `Task` contract is in `task_tests.cpp`.

#include "task_test_support.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cstddef>
#include <iostream>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CancelReason;
using tessera::runtime::ManualClock;
using tessera::runtime::TimerQueue;
using tessera::task_test::CompletionPriority;
using tessera::task_test::CompletionQueue;
using tessera::task_test::context;
using tessera::task_test::doubled;
using tessera::task_test::expect;
using tessera::task_test::marked;
using tessera::task_test::pump;
using tessera::task_test::Task;
using tessera::task_test::TaskCancelled;
using tessera::task_test::TaskContext;

// A parent that waits on a child, so completing the child has to resume it. That resume is a
// continuation and must survive a queue already full of plain completions (#843).
Task<int> parent_of(TaskContext ctx, int& out) {
    out = co_await doubled(ctx, 21);
    co_return out;
}

// Catches `TaskCancelled` at the await boundary and records the reason it carries (#845).
Task<void> catch_reason(TaskContext outer, TaskContext child, bool& ran, std::vector<int>& hits) {
    try {
        co_await marked(child, ran);
    } catch (const TaskCancelled& cancelled) {
        hits.push_back(static_cast<int>(cancelled.reason()));
    }
    (void)outer;
}

} // namespace

int main() {
    // --- #843: a full plain queue never drops a continuation ---------------------------------
    {
        // Capacity 1: one plain completion fills the fire-and-forget bound. The task's own resume
        // and its child's completion are continuations, so they are neither refused nor dropped.
        CompletionQueue queue(1);
        if (!expect(queue.post(CompletionPriority::Visible, [] {}), "the plain lane is full")) {
            return 1;
        }
        int out = 0;
        auto task = parent_of(context(queue), out);
        if (!expect(task.start(), "start on a full plain queue still queues the resume")) {
            return 1;
        }
        // The continuation lane drains before the plain lane, so the parent runs to its child await
        // and the child completes, all while the plain completion is still queued.
        std::size_t steps = 0;
        while (!task.done() && steps < 8) {
            steps += queue.run(1);
        }
        if (!expect(task.done(), "the task graph finished despite the full plain queue") ||
            !expect(out == 42, "the awaited value arrived") ||
            !expect(queue.stats().dropped == 0, "no resume was dropped") ||
            !expect(queue.stats().pending_continuations == 0, "no continuation is left queued") ||
            !expect(!queue.empty(), "the plain completion is still queued, untouched")) {
            return 2;
        }
        (void)queue.run(1);
        if (!expect(queue.empty(), "the plain completion finally ran")) {
            return 3;
        }
    }

    // --- #843: continuations drain before plain work at the same priority ---------------------
    {
        CompletionQueue queue(16);
        std::vector<char> order;
        for (int i = 0; i < 4; ++i) {
            queue.post(CompletionPriority::Visible, [&order] { order.push_back('p'); });
        }
        bool ran = false;
        auto task = marked(context(queue), ran);
        if (!expect(task.start(), "a task resume is queued behind four plain completions")) {
            return 4;
        }
        // One tick with budget 1 runs the task resume first even though it was posted last.
        if (!expect(queue.run(1) == 1 && ran, "the continuation beat the queued plain work")) {
            return 5;
        }
        (void)queue.run(8);
        if (!expect(order == std::vector<char>{'p', 'p', 'p', 'p'}, "the plain work ran after")) {
            return 6;
        }
    }

    // --- #845: the cancellation reason reaches the awaiter ------------------------------------
    {
        CompletionQueue queue(8);
        CancellationSource child_source;
        TaskContext child{&queue, CompletionPriority::Visible, child_source.token()};
        bool ran = false;
        std::vector<int> reasons;
        auto task = catch_reason(context(queue), child, ran, reasons);
        if (!expect(task.start(), "reason-catcher starts")) {
            return 7;
        }
        queue.run(1); // the parent runs to the child await, posting the child's first resume
        child_source.cancel(CancelReason::Timeout);
        pump(queue);
        if (!expect(reasons == std::vector<int>{static_cast<int>(CancelReason::Timeout)},
                    "the awaiter sees the Timeout reason") ||
            !expect(!ran, "the cancelled child never ran") || !expect(task.done(), "task done")) {
            return 7;
        }
    }
    {
        CancellationSource source;
        const auto token = source.token();
        source.cancel(CancelReason::Shutdown);
        if (!expect(token.cancelled(), "token cancelled") ||
            !expect(token.reason() == CancelReason::Shutdown, "the reason is Shutdown")) {
            return 8;
        }
        // The first reason wins.
        source.cancel(CancelReason::Timeout);
        if (!expect(token.reason() == CancelReason::Shutdown, "the first reason wins")) {
            return 9;
        }
    }

    // --- #845: a linked source is cancelled by either parent, with reason Parent --------------
    {
        CancellationSource first;
        CancellationSource second;
        CancellationSource linked = CancellationSource::linked(first.token(), second.token());
        const auto linked_token = linked.token();
        if (!expect(!linked_token.cancelled(), "a linked source starts live") ||
            !expect(first.token().registration_count() == 1,
                    "the link registered on the first parent") ||
            !expect(second.token().registration_count() == 1,
                    "the link registered on the second parent")) {
            return 10;
        }
        second.cancel();
        if (!expect(linked_token.cancelled(), "a parent cancels the linked child") ||
            !expect(linked_token.reason() == CancelReason::Parent, "the reason is Parent") ||
            !expect(first.token().registration_count() == 1,
                    "the other parent keeps its registration (it was consumed by the cancel)")) {
            return 11;
        }
        // Destroying the linked source unregisters from the remaining parent.
        {
            CancellationSource dying;
            const CancellationSource child = CancellationSource::linked(dying.token());
            if (!expect(dying.token().registration_count() == 1,
                        "the child registered on its parent")) {
                return 12;
            }
        }
        // A parent already cancelled at link time yields a cancelled child immediately.
        CancellationSource dead;
        dead.cancel();
        const CancellationSource already = CancellationSource::linked(dead.token());
        if (!expect(already.cancelled(), "a cancelled parent yields a cancelled child") ||
            !expect(already.token().reason() == CancelReason::Parent,
                    "the immediate link reason is Parent")) {
            return 13;
        }
    }

    // --- #845: cancel_after fires at the deadline under a ManualClock ------------------------
    {
        ManualClock clock(100.0);
        TimerQueue timers(4);
        CancellationSource source;
        const auto token = source.token();
        bool fired = false;
        auto registration = token.on_cancel([&fired] { fired = true; });
        if (!expect(source.cancel_after(clock, timers, 1.5), "cancel_after schedules a timer") ||
            !expect(timers.size() == 1, "one timer is scheduled")) {
            return 14;
        }
        (void)timers.fire_due(101.0);
        if (!expect(!fired && !token.cancelled(), "not due before the deadline")) {
            return 15;
        }
        (void)timers.fire_due(101.5);
        if (!expect(fired && token.cancelled(), "fires exactly at the deadline") ||
            !expect(token.reason() == CancelReason::Timeout, "the reason is Timeout") ||
            !expect(timers.size() == 0, "a one-shot removes itself")) {
            return 16;
        }
    }
    {
        // Re-arming moves the deadline; destroying the source removes its timer.
        ManualClock clock(0.0);
        TimerQueue timers(4);
        {
            CancellationSource source;
            if (!expect(source.cancel_after(clock, timers, 2.0),
                        "the first deadline is scheduled") ||
                !expect(source.cancel_after(clock, timers, 5.0), "re-arming schedules again")) {
                return 17;
            }
            if (!expect(timers.size() == 1, "re-arming replaces the timer")) {
                return 17;
            }
        }
        if (!expect(timers.size() == 0, "destroying the source removes the timer")) {
            return 18;
        }
    }
    {
        // A no-op on an already-cancelled source.
        ManualClock clock(0.0);
        TimerQueue timers(4);
        CancellationSource source;
        source.cancel();
        if (!expect(!source.cancel_after(clock, timers, 1.0),
                    "cancel_after after cancel is refused") ||
            !expect(timers.size() == 0, "no timer was scheduled")) {
            return 19;
        }
    }

    // --- #842: one wake per empty -> non-empty edge ------------------------------------------
    {
        int wakes = 0;
        auto wake = [](void* context_ptr) { ++*static_cast<int*>(context_ptr); };
        CompletionQueue queue(8, wake, &wakes);
        queue.post(CompletionPriority::Visible, [] {});
        queue.post(CompletionPriority::Visible, [] {});
        queue.post(CompletionPriority::Visible, [] {});
        if (!expect(wakes == 1, "a burst of posts wakes once")) {
            return 20;
        }
        (void)queue.run(8);
        queue.post(CompletionPriority::Visible, [] {});
        if (!expect(wakes == 2, "a post after draining wakes again")) {
            return 21;
        }
        (void)queue.run(8);
        bool ran = false;
        auto task = marked(context(queue), ran);
        if (!expect(task.start(), "the continuation is queued") ||
            !expect(wakes == 3, "a continuation post wakes the parked host")) {
            return 22;
        }
        pump(queue);
    }

    std::cout << "task_cancellation: ok\n";
    return 0;
}
