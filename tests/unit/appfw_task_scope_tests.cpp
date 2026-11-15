// SPDX-License-Identifier: MIT
//
// `appfw::TaskScope` part 1 (AF2, issue #174; ADR-050 section 5 item 1): the shape a service's
// background work lives inside - what a scope holds, how it cancels, and how a close sequence is
// reported without waiting.
//
// The roadmap row's acceptance clause for this half is "closing a scope cancels and drains". Cases:
//
//   * `close()` cancels every child and `tick()` reports the drain, so shutdown is observable
//     without waiting;
//   * a cancelled child's body never advances another step;
//   * a child that finishes on its own retires its slot and does not cancel the scope;
//   * a full scope refuses a spawn instead of growing.
//
// Suspension, the deadline and nesting are in `appfw_task_scope_suspension_tests.cpp`; the
// runtime's own `TaskCancelled` path and the refused-post case are in
// `appfw_task_scope_cancel_tests.cpp`.
//
// Deterministic: `ManualClock` for the deadline, one `CompletionQueue` drained by hand, no wall
// clock, no sleep, no thread, no network.

#include "appfw_task_scope_test_support.hpp"

namespace appfw_task_test {

namespace {

// Constructs a scope over the shared clock/queue with the requested capacity.
appfw::TaskScope make_scope(runtime::ManualClock& clock, runtime::CompletionQueue& queue,
                            std::size_t capacity = 32) {
    return appfw::TaskScope(clock, queue,
                            appfw::TaskScopeConfig{capacity, runtime::CompletionPriority::Visible});
}

} // namespace

// --- close() cancels and drains ----------------------------------------------------------------
bool close_cancels_and_drains() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    {
        appfw::TaskScope scope = make_scope(clock, queue);
        ChildLog first;
        ChildLog second;
        ok = expect(scope.spawn(hop_child(scope.context(), queue, first, 3)),
                    "the first child starts") &&
             ok;
        ok = expect(scope.spawn(hop_child(scope.context(), queue, second, 3)),
                    "the second child starts") &&
             ok;
        ok = expect(scope.live() == 2, "both children are live before the drain") && ok;

        // Let each child hop at least once, so the cancel lands mid-body rather than before the
        // first step: a cancel that only works pre-start would pass a weaker test. The stop
        // condition is a fact about the children (both advanced, neither finished), not a guess
        // about how many queue hops a spawn costs.
        pump_until(queue, [&] { return first.steps >= 1 && second.steps >= 1; });
        const int steps_before = first.steps + second.steps;
        ok = expect(steps_before >= 1, "a child made progress before the close") && ok;
        ok = expect(scope.live() == 2, "both children are still live mid-body") && ok;

        ok = expect(!scope.drained(), "the scope is not drained while children are live") && ok;
        ok = expect(scope.tick() == appfw::TaskScopeCloseState::Open,
                    "tick() reports Open before close()") &&
             ok;

        scope.close(clock.now_seconds() + 1.0);
        ok = expect(scope.cancelled(), "close() cancels the scope") && ok;
        ok = expect(scope.closing(), "close() marks the scope closing") && ok;
        ok = expect(scope.tick() == appfw::TaskScopeCloseState::Closing,
                    "tick() reports Closing while children wind down") &&
             ok;

        // The queued resumes still run, but each child sees the token and returns. This is the
        // drain, and it is what makes `Closed` reachable without waiting on anything.
        pump(queue);
        ok = expect(scope.drained(), "the drain retires every child") && ok;
        ok = expect(scope.live() == 0, "live count is zero after the drain") && ok;
        ok = expect(scope.finished() == 2, "both children finished") && ok;
        ok = expect(first.completions == 0 && second.completions == 0,
                    "a cancelled child never reached its normal completion") &&
             ok;
        // These children observe the token themselves after their hop resumes, so they exit by
        // returning: cooperative cancellation, which is the path the runtime is designed around.
        // The `TaskCancelled` path (the runtime skipping a resume) is covered separately by
        // `a_child_cancelled_before_its_first_step_is_counted`.
        ok = expect(scope.cancelled_children() == 0,
                    "a child that returns on its own token check is not counted as skipped") &&
             ok;
        ok = expect(scope.failed() == 0, "cancellation is not counted as a failure") && ok;
        ok = expect(first.steps + second.steps == steps_before,
                    "a cancelled child's body ran no further step") &&
             ok;
        ok = expect(scope.tick() == appfw::TaskScopeCloseState::Closed,
                    "tick() reports Closed once drained") &&
             ok;

        // A closed scope refuses new work; that refusal is what makes `close()` final.
        ChildLog late;
        ok = expect(!scope.spawn(hop_child(scope.context(), queue, late, 1)),
                    "a closed scope refuses a spawn") &&
             ok;
        ok = expect(scope.refused() == 1, "the refusal is counted") && ok;
    }
    return ok;
}

// --- a child that finishes on its own retires its slot -----------------------------------------
bool a_finished_child_retires_its_slot() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope scope = make_scope(clock, queue);
    ChildLog log;
    ok = expect(scope.spawn(hop_child(scope.context(), queue, log, 2)), "the child starts") && ok;
    ok = expect(scope.live() == 1, "the child is live") && ok;
    pump(queue);
    ok = expect(log.completions == 1, "the child ran to completion") && ok;
    ok = expect(scope.live() == 0, "a finished child is no longer live") && ok;
    ok = expect(scope.finished() == 1, "the finish is counted") && ok;
    ok = expect(!scope.cancelled(), "finishing normally does not cancel the scope") && ok;
    ok = expect(scope.tick() == appfw::TaskScopeCloseState::Open,
                "an unclosed scope still reports Open") &&
         ok;
    return ok;
}

// --- capacity is a bound, not a hint ------------------------------------------------------------
bool capacity_refuses_rather_than_grows() {
    bool ok = true;
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(64);
    appfw::TaskScope scope = make_scope(clock, queue, 2);
    ChildLog first;
    ChildLog second;
    ChildLog third;
    ok = expect(scope.spawn(parked_child(scope.context(), first)), "spawn 1 of 2") && ok;
    ok = expect(scope.spawn(parked_child(scope.context(), second)), "spawn 2 of 2") && ok;
    ok = expect(!scope.spawn(parked_child(scope.context(), third)), "spawn 3 is refused") && ok;
    ok = expect(scope.live() == 2, "the refusals did not add a live child") && ok;
    ok = expect(scope.peak() == 2, "peak never exceeded capacity") && ok;
    ok = expect(scope.capacity() == 2, "capacity is reported") && ok;
    return ok;
}

int run_lifecycle_tests() {
    const int before = failures;
    close_cancels_and_drains();
    a_finished_child_retires_its_slot();
    capacity_refuses_rather_than_grows();
    return failures - before;
}

} // namespace appfw_task_test
