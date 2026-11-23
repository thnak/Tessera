// SPDX-License-Identifier: MIT
//
// `appfw::TaskScope` part 3 (AF2, issue #174): the paths where a child is ended or never started by
// something other than its own token check.
//
//   * the runtime's `TaskCancelled` path: a resume that was already queued when the scope was
//     cancelled is skipped, so the child finishes without executing another statement. The driver's
//     `catch (const runtime::TaskCancelled&)` branch exists for this and counts it separately from
//     a cooperative return, so an app can tell "exited because it checked" from "never ran";
//   * a child whose first resume the completion queue refuses: nothing ran, so the slot is released
//     and the attempt is counted as refused rather than live;
//   * a child that throws: the exception becomes a counter at the scope boundary and never reaches
//     the frame loop (ADR-025).
//
// The close/drain path is in `appfw_task_scope_tests.cpp`; suspension and nesting are in
// `appfw_task_scope_suspension_tests.cpp`.

#include "appfw_task_scope_test_support.hpp"

namespace appfw_task_test {

namespace {

appfw::TaskScope make_scope(runtime::ManualClock& clock, runtime::CompletionQueue& queue,
                            std::size_t capacity = 32) {
    return appfw::TaskScope(clock, queue,
                            appfw::TaskScopeConfig{capacity, runtime::CompletionPriority::Visible});
}

} // namespace

// --- a child's exception becomes a counter -----------------------------------------------------
bool a_throwing_child_is_counted_not_propagated() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope scope = make_scope(clock, queue);
    ChildLog log;
    ok = expect(scope.spawn(throwing_child(scope.context(), log)), "the throwing child starts") &&
         ok;
    pump(queue);
    ok = expect(log.entries == 1, "the throwing child ran") && ok;
    ok = expect(scope.failed() == 1, "the exception was counted at the scope boundary") && ok;
    ok = expect(scope.live() == 0, "the failed child was retired") && ok;
    ok = expect(scope.drained(), "a failed child leaves the scope drained") && ok;
    return ok;
}

// --- the runtime's own cancellation path ------------------------------------------------------
bool a_child_cancelled_before_its_first_step_is_counted() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope scope = make_scope(clock, queue);
    ChildLog skipped;
    ok = expect(scope.spawn(hop_child(scope.context(), queue, skipped, 1)),
                "the child task is created and started") &&
         ok;
    ok = expect(skipped.entries == 0, "the child body has not run before the queue drains") && ok;

    // Cancel before any drain: the queued resume is still pending, so the runtime skips it and the
    // child finishes with `TaskCancelled` without executing a single statement of its body.
    scope.cancel();
    pump(queue);
    ok = expect(skipped.entries == 0, "a skipped child never entered its body") && ok;
    ok = expect(skipped.steps == 0, "a skipped child ran no step") && ok;
    ok = expect(scope.live() == 0, "the skipped child was retired") && ok;
    ok = expect(scope.finished() == 1, "the skipped child counts as finished") && ok;
    ok = expect(scope.cancelled_children() == 1,
                "the skip was reported through the runtime's TaskCancelled path") &&
         ok;
    ok = expect(scope.failed() == 0, "a skip is not a failure") && ok;
    return ok;
}

// --- a spawn is accepted even when the fire-and-forget queue is full ---------------------------
// Issue #843: a task resume goes on the unbounded continuation lane, so a full plain-completion
// queue can no longer refuse a spawn (which used to leave nothing live and refuse the child).
bool a_full_queue_still_accepts_a_spawn() {
    bool ok = true;
    runtime::ManualClock clock;
    // A queue with capacity 1 is filled with an unrelated plain completion. Before #843 the
    // driver's first resume was refused here; now it is queued on the continuation lane and runs.
    runtime::CompletionQueue queue(1);
    appfw::TaskScope scope = make_scope(clock, queue);
    ok =
        expect(queue.post(runtime::CompletionPriority::Visible, [] {}), "the plain lane is full") &&
        ok;
    ChildLog log;
    ok = expect(scope.spawn(hop_child(scope.context(), queue, log, 0)),
                "a full plain lane does not refuse a spawn") &&
         ok;
    ok = expect(scope.live() == 1, "the spawned child is live") && ok;
    ok = expect(scope.spawned() == 1, "the spawn is counted") && ok;

    // The continuation lanes drain before the plain lane at the same priority, even though the
    // plain completion was posted first. A zero-step child needs no further plain post, so the
    // capacity-1 plain lane stays full and nothing drops.
    ok = expect(queue.pending(runtime::CompletionPriority::Visible) == 2,
                "the driver's resume is queued next to the plain completion") &&
         ok;
    ok = expect(queue.run(1) == 1, "the driver resume drains first") && ok;
    ok = expect(queue.run(1) == 1 && log.entries == 1,
                "the child body ran from the continuation lane") &&
         ok;
    ok = expect(scope.refused() == 0, "nothing was refused") && ok;
    // The driver's completion resumes before the plain completion, so the child retires while the
    // plain work is still queued: the plain lane was never needed to make progress.
    ok = expect(queue.run(1) == 1 && scope.live() == 0, "the driver retires the slot") && ok;
    ok = expect(!queue.empty(), "the plain completion is still queued") && ok;
    return ok;
}

int run_cancel_tests() {
    const int before = failures;
    a_throwing_child_is_counted_not_propagated();
    a_child_cancelled_before_its_first_step_is_counted();
    a_full_queue_still_accepts_a_spawn();
    return failures - before;
}

} // namespace appfw_task_test
