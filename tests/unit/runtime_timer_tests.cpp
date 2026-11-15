// SPDX-License-Identifier: MIT
//
// The layer-0 timer wake source, part 1 (AF2, issue #174; ADR-050 section 5 item 2): `TimerQueue`'s
// ordering, handle safety and error contract.
//
// The acceptance clause this half proves is the issue's first one, "timers fire in order" -
// earliest deadline first, and equal deadlines broken by registration order, deterministically. The
// second clause ("an idle loop with only a far timer parks, zero presents, no polling") lives in
// `runtime_timer_park_tests.cpp`.
//
// Deterministic: deadlines are absolute instants handed to `fire_due`, so nothing here reads a
// clock, sleeps, spins or starts a thread.

#include "runtime_timer_test_support.hpp"

namespace runtime_timer_test {

bool timers_fire_in_deadline_order() {
    bool ok = true;
    TimerQueue queue(8);
    std::vector<int> fired;

    // Registered out of deadline order on purpose: registration order must not leak into the fire
    // order when the deadlines differ.
    const auto late = queue.schedule_one_shot(30.0, [&] { fired.push_back(3); });
    const auto early = queue.schedule_one_shot(10.0, [&] { fired.push_back(1); });
    const auto middle = queue.schedule_one_shot(20.0, [&] { fired.push_back(2); });
    ok = expect(late.has_value() && early.has_value() && middle.has_value(),
                "three timers were scheduled") &&
         ok;

    // Before its deadline nothing fires, and the earliest deadline is the one reported.
    ok = expect(queue.fire_due(9.999) == 0, "no timer fires before its deadline") && ok;
    ok = expect(near(queue.next_deadline_seconds(), 10.0),
                "the earliest deadline is what the loop reports") &&
         ok;

    ok = expect(queue.fire_due(10.0) == 1, "one timer fires at the first deadline") && ok;
    ok = expect(fired == std::vector<int>{1}, "only the 10 s timer fired at 10 s") && ok;
    ok = expect(near(queue.next_deadline_seconds(), 20.0), "the next deadline advanced") && ok;

    // A single call at a late instant fires the remaining timers in deadline order, not in slot
    // order, and not by insertion.
    ok = expect(queue.fire_due(30.0) == 2, "the remaining two fire in one call") && ok;
    const std::vector<int> expected{1, 2, 3};
    ok = expect(fired == expected, "timers fired in deadline order") && ok;
    ok = expect(queue.size() == 0, "every one-shot timer is gone after firing") && ok;
    ok = expect(queue.next_deadline_seconds() == kNoDeadline,
                "an empty queue reports no deadline") &&
         ok;
    return ok;
}

// The literal acceptance assertion: equal deadlines break by registration order.
bool equal_deadlines_break_by_registration_order() {
    bool ok = true;
    TimerQueue queue(8);
    std::vector<int> fired;

    // Registered in a deliberately scrambled "expected" order, all due at the same instant.
    const auto third = queue.schedule_one_shot(5.0, [&] { fired.push_back(3); });
    const auto first = queue.schedule_one_shot(5.0, [&] { fired.push_back(1); });
    const auto second = queue.schedule_one_shot(5.0, [&] { fired.push_back(2); });
    ok = expect(third && first && second, "three equal-deadline timers were scheduled") && ok;

    ok = expect(queue.fire_due(5.0) == 3, "all three fire at their shared deadline") && ok;
    const std::vector<int> expected{3, 1, 2};
    ok = expect(fired == expected, "equal deadlines fire in registration order (3, 1, 2)") && ok;

    // Determinism is asserted the same way a second time: a fresh queue with the same registration
    // sequence produces the same vector, so the order is a property of the queue and not of this
    // run's slot allocation.
    TimerQueue repeat(8);
    std::vector<int> repeated;
    (void)repeat.schedule_one_shot(5.0, [&] { repeated.push_back(3); });
    (void)repeat.schedule_one_shot(5.0, [&] { repeated.push_back(1); });
    (void)repeat.schedule_one_shot(5.0, [&] { repeated.push_back(2); });
    (void)repeat.fire_due(5.0);
    ok = expect(repeated == expected, "the registration order is reproducible") && ok;

    // A mix: two timers share the earlier deadline, a third is later. Order is (deadline,
    // sequence), which is the comparator the queue implements.
    TimerQueue mixed(8);
    std::vector<int> order;
    (void)mixed.schedule_one_shot(2.0, [&] { order.push_back(20); });
    (void)mixed.schedule_one_shot(1.0, [&] { order.push_back(10); });
    (void)mixed.schedule_one_shot(2.0, [&] { order.push_back(21); });
    (void)mixed.fire_due(2.0);
    ok = expect(order == std::vector<int>{10, 20, 21},
                "the ordering is (deadline, registration), not registration alone") &&
         ok;
    return ok;
}

bool periodic_timers_coalesce_missed_periods() {
    bool ok = true;
    TimerQueue queue(4);
    int fires = 0;
    const auto id = queue.schedule_periodic(1.0, 0.5, [&] { ++fires; });
    ok = expect(id.has_value(), "the periodic timer was scheduled") && ok;

    (void)queue.fire_due(1.0);
    ok = expect(fires == 1, "the first period fires at the deadline") && ok;
    ok = expect(near(queue.next_deadline_seconds(), 1.5), "the next period is one period later") &&
         ok;

    // 3.0 is four periods late: the timer fires once and advances past `now`, so a loop that parks
    // through a long gap is not woken by a burst of catch-up callbacks.
    (void)queue.fire_due(3.0);
    ok = expect(fires == 2, "a missed period fires once, not once per missed period") && ok;
    ok = expect(queue.next_deadline_seconds() > 3.0,
                "the coalesced deadline moved past the current instant") &&
         ok;

    ok = expect(queue.cancel(*id), "a periodic timer can be cancelled") && ok;
    ok = expect(queue.fire_due(100.0) == 0, "a cancelled timer never fires again") && ok;
    ok = expect(!queue.cancel(*id), "cancelling twice is refused") && ok;
    return ok;
}

bool stale_and_reused_handles_are_safe() {
    bool ok = true;
    TimerQueue queue(1);
    const auto first = queue.schedule_one_shot(1.0, [] {});
    ok = expect(first.has_value(), "the slot was assigned") && ok;
    ok = expect(!queue.schedule_one_shot(2.0, [] {}).has_value(),
                "a full queue refuses instead of growing") &&
         ok;
    (void)queue.fire_due(1.0);
    // The one-slot queue has freed its slot; a new timer reuses it. The old handle must not cancel
    // the new timer.
    const auto second = queue.schedule_one_shot(9.0, [] {});
    ok = expect(second.has_value(), "the slot is reused") && ok;
    ok = expect(second->slot == first->slot, "the reuse is the same slot") && ok;
    ok =
        expect(second->generation != first->generation, "the generation distinguishes the reuse") &&
        ok;
    ok = expect(!queue.cancel(*first), "a stale handle cancels nothing") && ok;

    int fired = 0;
    TimerQueue counting(1);
    const auto live = counting.schedule_one_shot(9.0, [&] { ++fired; });
    ok = expect(live.has_value(), "the live timer was scheduled") && ok;
    ok = expect(!counting.cancel(TimerId{live->slot, live->generation + 7}),
                "a wrong-generation handle cancels nothing") &&
         ok;
    (void)counting.fire_due(9.0);
    ok = expect(fired == 1, "the live timer survived the stale cancels") && ok;
    return ok;
}

bool invalid_schedules_are_rejected() {
    bool ok = true;
    TimerQueue queue(4);
    ok =
        expect(throws_invalid_argument([&] { (void)queue.schedule_one_shot(std::nan(""), [] {}); }),
               "a non-finite deadline is rejected") &&
        ok;
    ok = expect(throws_invalid_argument([&] { (void)queue.schedule_periodic(1.0, 0.0, [] {}); }),
                "a non-positive period is rejected") &&
         ok;
    ok = expect(throws_invalid_argument(
                    [&] { (void)queue.schedule_one_shot(1.0, std::function<void()>{}); }),
                "an empty callback is rejected") &&
         ok;

    // A non-finite instant fires nothing rather than throwing on the frame path.
    (void)queue.schedule_one_shot(1.0, [] {});
    ok = expect(queue.fire_due(std::nan("")) == 0, "a non-finite now fires nothing") && ok;
    ok = expect(queue.fire_due(kNoDeadline) == 0, "an infinite now fires nothing") && ok;
    return ok;
}

int run_ordering_tests() {
    const int before = failures;
    timers_fire_in_deadline_order();
    equal_deadlines_break_by_registration_order();
    periodic_timers_coalesce_missed_periods();
    stale_and_reused_handles_are_safe();
    invalid_schedules_are_rejected();
    return failures - before;
}

} // namespace runtime_timer_test
