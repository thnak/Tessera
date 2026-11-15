// SPDX-License-Identifier: MIT
//
// Observable<T> over UpdateScheduler (issue #176, ADR-050 section 6 and roadmap row AF4). This file
// carries the acceptance proof: N `set()` calls between two ticks produce exactly ONE observer
// notification, and an unchanged write produces none at all.
//
// Deterministic by construction: no wall clock, no sleeps, no threads. The flush is a direct
// `UpdateScheduler::flush()` call, so "one flush per tick" is a pure assertion. Subscription and
// delivery semantics live in `appfw_observable_subscription_tests.cpp`; the mandatory
// allocation-counting proof lives in `appfw_observable_allocation.cpp`.

#include "appfw_observable_test_support.hpp"

namespace appfw_test {

namespace {

// The acceptance clause, first and literally: N sets between ticks -> ONE notification.
bool n_sets_between_ticks_produce_one_notification() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Recorder recorder;
    auto subscription = observable.subscribe(&Recorder::observe, &recorder);

    constexpr int kSets = 500;
    for (int i = 1; i <= kSets; ++i) {
        observable.set(i);
    }
    bool ok = expect(recorder.calls == 0,
                     "set() never notifies inline; the first notification waits for the flush");
    ok = expect(observable.get() == kSets, "the value is the last one written") && ok;
    ok = expect(observable.version() == kSets, "every accepted change advanced the version") && ok;
    ok = expect(observable.dirty(), "the observable is pending before the tick") && ok;

    const ui::UpdateFlushStats stats = scheduler.flush();
    ok = expect(recorder.calls == 1, "500 set() calls produced exactly one notification: got " +
                                         std::to_string(recorder.calls)) &&
         ok;
    ok = expect(stats.updated == 1, "the scheduler ran exactly one update in that flush: got " +
                                        std::to_string(stats.updated)) &&
         ok;
    ok = expect(recorder.values.size() == 1 && recorder.values[0] == kSets,
                "the single notification carried the final value, not an intermediate one") &&
         ok;
    ok = expect(!observable.dirty(), "the dirty bit cleared") && ok;

    // A tick with no write must not notify, and a second flush is a no-op.
    ok = expect(scheduler.flush().updated == 0, "an idle tick notifies nothing") && ok;
    ok =
        expect(recorder.calls == 1, "the notification count is still one after an idle tick") && ok;

    // One set -> one notification per tick: the same contract at the other end of the range.
    observable.set(-1);
    scheduler.flush();
    ok = expect(recorder.calls == 2 && recorder.values.back() == -1,
                "one set produces one notification on the next tick") &&
         ok;
    (void)subscription;
    return ok;
}

// A hover handler that re-asserts the current value must not dirty the scheduler at all.
bool setting_the_same_value_is_free() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 7);
    Recorder recorder;
    auto subscription = observable.subscribe(&Recorder::observe, &recorder);

    bool ok = expect(!observable.set(7), "set() with an equal value reports no change");
    ok = expect(!observable.dirty(), "an equal value does not schedule a flush") && ok;
    ok = expect(scheduler.flush().updated == 0, "and the tick does nothing") && ok;
    ok = expect(recorder.calls == 0, "so no observer heard anything") && ok;
    ok = expect(observable.version() == 0, "the version counts accepted changes only") && ok;
    (void)subscription;
    return ok;
}

// Many observers hear the same flush once each, and they all see the same value.
bool every_observer_is_notified_once_per_flush() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Recorder first;
    Recorder second;
    Recorder third;
    auto subscriptions = std::vector<appfw::Observable<int>::Subscription>{};
    subscriptions.push_back(observable.subscribe(&Recorder::observe, &first));
    subscriptions.push_back(observable.subscribe(&Recorder::observe, &second));
    subscriptions.push_back(observable.subscribe(&Recorder::observe, &third));

    for (int i = 1; i <= 10; ++i) {
        observable.set(i);
    }
    scheduler.flush();
    bool ok = expect(first.calls == 1 && second.calls == 1 && third.calls == 1,
                     "three observers, three notifications (one each), not thirty");
    ok = expect(first.values == second.values && second.values == third.values,
                "every observer saw the same value") &&
         ok;
    ok = expect(observable.observer_count() == 3, "the registry holds all three") && ok;
    return ok;
}

// A second subscription added between two ticks participates from the next flush on, and does not
// retroactively receive the previous change.
bool a_late_observer_starts_at_the_next_flush() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Recorder early;
    Recorder late;
    auto first = observable.subscribe(&Recorder::observe, &early);

    observable.set(1);
    scheduler.flush();
    auto second = observable.subscribe(&Recorder::observe, &late);
    scheduler.flush();
    bool ok = expect(early.calls == 1 && late.calls == 0,
                     "a flush with nothing dirty notifies neither observer, including a new one");

    observable.set(2);
    scheduler.flush();
    ok = expect(early.calls == 2 && late.calls == 1, "both hear the next change once") && ok;
    (void)first;
    (void)second;
    return ok;
}

} // namespace

} // namespace appfw_test

int main() {
    const bool allocation_ok = appfw_test::observable_set_allocates_nothing();
    bool ok = appfw_test::n_sets_between_ticks_produce_one_notification();
    ok = appfw_test::setting_the_same_value_is_free() && ok;
    ok = appfw_test::every_observer_is_notified_once_per_flush() && ok;
    ok = appfw_test::a_late_observer_starts_at_the_next_flush() && ok;
    ok = appfw_test::subscription_semantics() && ok;
    if (!ok || appfw_test::failures != 0 || !allocation_ok) {
        std::cerr << appfw_test::failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "appfw_observable: ok\n";
    return 0;
}
