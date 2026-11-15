// SPDX-License-Identifier: MIT
//
// `appfw::TaskScope` part 2 (AF2, issue #174): the cases where a child is *suspended* when the
// scope goes away, and where scopes nest.
//
// These are the cases that decide the design, because a suspended coroutine frame may be referenced
// by a bare `coroutine_handle` inside the awaitable it is parked on. A scope must therefore never
// destroy such a frame:
//
//   * destroying the `TaskScope` handle while a child is parked is safe - the child's state
//   survives
//     as long as the task graph refers to it, and no resume is left pending;
//   * a close deadline that expires with a child still parked reports `TimedOut` and retains it,
//     rather than pretending it drained or destroying it;
//   * cancelling a parent scope cancels a child scope through the `CancellationToken` chain.
//
// The close/drain path is in `appfw_task_scope_tests.cpp`; the runtime's `TaskCancelled` path is in
// `appfw_task_scope_cancel_tests.cpp`.

#include "appfw_task_scope_test_support.hpp"

namespace appfw_task_test {

namespace {

appfw::TaskScope make_scope(runtime::ManualClock& clock, runtime::CompletionQueue& queue,
                            std::size_t capacity = 32) {
    return appfw::TaskScope(clock, queue,
                            appfw::TaskScopeConfig{capacity, runtime::CompletionPriority::Visible});
}

} // namespace

// --- destroying the handle while a child is parked ---------------------------------------------
//
// The point of this case is not that the frame is freed (a parked child cannot be freed safely),
// but that nothing crashes, the scope's state survives as long as its children refer to it, and the
// counters show the abandonment instead of hiding it. A destructor that destroyed suspended frames
// would fail here under ASan and, in production, would corrupt the completer's handle.
bool destroy_while_suspended_is_safe() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    ChildLog log;
    {
        appfw::TaskScope scope = make_scope(clock, queue);
        ok = expect(scope.spawn(parked_child(scope.context(), log)), "the parked child starts") &&
             ok;
        // The spawn posts the driver's first resume; the child body runs on the drain, which is
        // what makes "entered its body" an observable fact rather than a guess about inline starts.
        pump_until(queue, [&] { return log.entries == 1; });
        ok = expect(log.entries == 1, "the child entered its body") && ok;
        ok = expect(scope.live() == 1, "the parked child is live") && ok;
        // The scope is destroyed here with the child still suspended inside `ParkForever`.
    }
    ok = expect(log.completions == 0, "a parked child never completed") && ok;
    // Nothing may resume from this queue: the child is parked on an await that never posts. The
    // pump is here to prove the queue is empty and stayed quiet.
    ok = expect(pump(queue) == 0, "no resume was pending after the scope was destroyed") && ok;
    return ok;
}

// --- the deadline is advisory, and never destroys a suspended child ----------------------------
bool deadline_reports_timed_out_without_destroying() {
    bool ok = true;
    runtime::ManualClock clock(100.0);
    runtime::CompletionQueue queue(64);
    appfw::TaskScope scope = make_scope(clock, queue);
    ChildLog log;
    ok = expect(scope.spawn(parked_child(scope.context(), log)), "the parked child starts") && ok;

    scope.close(clock.now_seconds() + 0.5);
    ok = expect(scope.tick() == appfw::TaskScopeCloseState::Closing,
                "the close sequence is Closing before the deadline") &&
         ok;
    clock.advance(0.5);
    ok = expect(scope.tick() == appfw::TaskScopeCloseState::TimedOut,
                "the deadline reports TimedOut with a child still live") &&
         ok;
    ok = expect(scope.live() == 1, "the parked child is retained, not destroyed") && ok;
    ok = expect(scope.tick() == appfw::TaskScopeCloseState::TimedOut,
                "TimedOut keeps being reported while the child is still live") &&
         ok;
    ok = expect(!scope.drained(), "a timed-out scope is not drained") && ok;
    // A second close must not shorten or revive the sequence; the latch outranks the new deadline.
    scope.close(clock.now_seconds() + 10.0);
    ok = expect(scope.tick() == appfw::TaskScopeCloseState::TimedOut,
                "a second close() does not revive a timed-out scope") &&
         ok;
    ok = expect(scope.live() == 1, "the parked child is still retained after the second close") &&
         ok;
    return ok;
}

// --- nesting -----------------------------------------------------------------------------------
bool cancelling_a_parent_cancels_a_child_scope() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope parent = make_scope(clock, queue);
    appfw::TaskScope child(clock, queue,
                           appfw::TaskScopeConfig{4, runtime::CompletionPriority::Visible},
                           parent.context().token);
    ChildLog log;
    ok = expect(child.spawn(parked_child(child.context(), log)), "the child scope spawns") && ok;
    ok = expect(!child.cancelled(), "the child scope starts uncancelled") && ok;

    parent.cancel();
    ok = expect(child.cancelled(), "cancelling the parent cancelled the child scope") && ok;
    ok = expect(child.spawn(parked_child(child.context(), log)) == false,
                "a cancelled child scope refuses new work") &&
         ok;
    return ok;
}

// A child scope that outlives its parent must not be cancelled later through a dangling
// registration: the parent's callback holds the child's state weakly, so the callback finds nothing
// instead of writing through freed memory. This case is the crash-proof for that rule.
bool a_child_scope_outliving_its_parent_is_safe() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope parent = make_scope(clock, queue);
    auto child = std::make_unique<appfw::TaskScope>(
        clock, queue, appfw::TaskScopeConfig{4, runtime::CompletionPriority::Visible},
        parent.context().token);
    ok = expect(!child->cancelled(), "the child scope starts uncancelled") && ok;
    child.reset();
    // The parent is cancelled with no live child scope; the weak registration must be a no-op.
    parent.cancel();
    ok = expect(parent.cancelled(), "the parent still cancels normally") && ok;
    return ok;
}

int run_suspension_tests() {
    const int before = failures;
    destroy_while_suspended_is_safe();
    deadline_reports_timed_out_without_destroying();
    cancelling_a_parent_cancels_a_child_scope();
    a_child_scope_outliving_its_parent_is_safe();
    return failures - before;
}

} // namespace appfw_task_test
