// SPDX-License-Identifier: MIT
//
// `Observable<T>` subscription semantics (issue #176): the `const T&` delivery model, immediate
// removal with deferred reclamation when an observer unsubscribes during a notification, the RAII
// `Subscription` (move, release, outliving its observable), the `UpdateTicket` observer form, and
// the scheduler-supplied re-entrancy bound.
//
// Deterministic: no wall clock, no sleeps, no threads; each case flushes a plain
// `UpdateScheduler`. The coalescing proof is in `appfw_observable_tests.cpp` and the allocation
// proof is in `appfw_observable_allocation.cpp`.

#include "appfw_observable_test_support.hpp"

#include <utility>
#include <vector>

namespace appfw_test {

namespace {

// ---------------------------------------------------------------------------------------------
// Delivery model: `const T&` bound to the live value, no copy per observer.
// ---------------------------------------------------------------------------------------------

// A type that counts copies: the observer receives a reference to the observable's own value, so a
// pass adds no copies at all. A copy-per-observer model would add one per observer per
// notification, which is the cost this model exists to avoid.
struct CountedValue {
    static int copies;
    static int constructions;

    int payload = 0;

    explicit CountedValue(int value)
        : payload(value) {
        ++constructions;
    }
    CountedValue(const CountedValue& other)
        : payload(other.payload) {
        ++copies;
    }
    CountedValue& operator=(const CountedValue& other) {
        payload = other.payload;
        ++copies;
        return *this;
    }
    CountedValue(CountedValue&& other) noexcept
        : payload(other.payload) {
        ++constructions;
    }
    CountedValue& operator=(CountedValue&& other) noexcept {
        payload = other.payload;
        ++constructions;
        return *this;
    }
    ~CountedValue() = default;

    bool operator==(const CountedValue& other) const {
        return payload == other.payload;
    }
};

int CountedValue::copies = 0;
int CountedValue::constructions = 0;

bool delivery_is_a_reference_to_the_live_value() {
    struct Seen {
        const CountedValue* address = nullptr;
        int payload = 0;
    };
    ui::UpdateScheduler scheduler;
    appfw::Observable<CountedValue> observable(scheduler, CountedValue(1));
    Seen seen;
    const auto observe = [](void* context, const CountedValue& value) {
        auto* state = static_cast<Seen*>(context);
        state->address = &value;
        state->payload = value.payload;
    };
    auto subscription = observable.subscribe(observe, &seen);

    CountedValue::copies = 0;
    const int constructions_before = CountedValue::constructions;
    for (int i = 2; i <= 20; ++i) {
        observable.set(CountedValue(i));
    }
    // Each `set` compares (no copy), then move-assigns: 19 sets, 0 copies.
    bool ok = expect(CountedValue::copies == 0,
                     "set() copies nothing: " + std::to_string(CountedValue::copies) + " copies");
    scheduler.flush();
    ok = expect(seen.address == &observable.get(),
                "the observer received a reference to the observable's live value, not a copy") &&
         ok;
    ok = expect(seen.payload == 20, "and it read the current payload") && ok;
    ok = expect(CountedValue::copies == 0, "the notification pass copied the value zero times") &&
         ok;
    ok = expect(CountedValue::constructions - constructions_before >= 19,
                "the value moved through set(), never duplicated for an observer") &&
         ok;
    ok = expect(observable.observer_count() == 1, "one observer, one link") && ok;
    (void)subscription;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Unsubscribe semantics: immediate removal, deferred reclamation, safe from inside a callback.
// ---------------------------------------------------------------------------------------------

// An observer that unsubscribes itself during its own notification is not called again, while the
// observer after it still runs and the pass completes.
bool self_unsubscribe_stops_after_the_current_call() {
    struct SelfRemover {
        appfw::Observable<int>::Subscription subscription;
        int calls = 0;
    };
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    int third_calls = 0;
    SelfRemover remover;
    remover.subscription = observable.subscribe(
        [](void* context, const int&) {
            auto* self = static_cast<SelfRemover*>(context);
            ++self->calls;
            self->subscription.unsubscribe();
        },
        &remover);
    auto third = observable.subscribe(
        [](void* context, const int&) { ++(*static_cast<int*>(context)); }, &third_calls);

    observable.set(1);
    scheduler.flush();
    bool ok = expect(remover.calls == 1, "the self-unsubscribing observer ran once");
    ok =
        expect(!remover.subscription.active(), "and its subscription is inactive afterwards") && ok;
    ok = expect(third_calls == 1, "the observer after the removed slot still ran") && ok;
    ok =
        expect(observable.observer_count() == 1, "the registry no longer holds the removed link") &&
        ok;

    observable.set(2);
    scheduler.flush();
    ok = expect(remover.calls == 1, "it is not called on the next tick either") && ok;
    ok = expect(third_calls == 2, "the survivor keeps receiving") && ok;
    (void)third;
    return ok;
}

// An observer that unsubscribes a *later* observer prevents that one from being called in the same
// pass (immediate removal); everyone else in the pass is unaffected.
bool unsubscribe_of_a_later_observer_takes_effect_in_the_same_pass() {
    struct Killer {
        appfw::Observable<int>::Subscription* target = nullptr;
        int calls = 0;
    };
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    int second_calls = 0;
    int third_calls = 0;
    appfw::Observable<int>::Subscription second_subscription;

    Killer killer{&second_subscription, 0};
    auto first = observable.subscribe(
        [](void* context, const int&) {
            auto* self = static_cast<Killer*>(context);
            ++self->calls;
            self->target->unsubscribe();
        },
        &killer);
    second_subscription = observable.subscribe(
        [](void* context, const int&) { ++(*static_cast<int*>(context)); }, &second_calls);
    auto third = observable.subscribe(
        [](void* context, const int&) { ++(*static_cast<int*>(context)); }, &third_calls);

    observable.set(1);
    scheduler.flush();
    bool ok = expect(killer.calls == 1, "the first observer ran");
    ok = expect(second_calls == 0,
                "the observer unsubscribed earlier in the same pass was NOT called: immediate "
                "removal") &&
         ok;
    ok = expect(third_calls == 1, "the pass continued past the removed slot") && ok;
    ok = expect(observable.observer_count() == 2, "the removed link is gone from the registry") &&
         ok;

    // And it stays gone for later ticks.
    observable.set(2);
    scheduler.flush();
    ok = expect(second_calls == 0, "a removed observer is never called again") && ok;
    ok = expect(killer.calls == 2 && third_calls == 2, "the survivors keep receiving") && ok;
    (void)first;
    (void)third;
    return ok;
}

// Unsubscribing outside a notification removes the link and frees the node immediately: the RAII
// subscription is the mount/unmount path, so this is the common case.
bool unsubscribe_outside_a_flush_removes_immediately() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Recorder recorder;
    {
        auto subscription = observable.subscribe(&Recorder::observe, &recorder);
        expect(observable.observer_count() == 1, "the link exists");
        observable.set(1);
        scheduler.flush();
        expect(recorder.calls == 1, "it heard the first change");
    }
    bool ok =
        expect(observable.observer_count() == 0, "the subscription's destructor removed the link");
    observable.set(2);
    scheduler.flush();
    ok = expect(recorder.calls == 1, "a destroyed subscription hears nothing afterwards") && ok;
    return ok;
}

// Moving a subscription transfers the link: the moved-from one must not unsubscribe the link the
// moved-to one now owns.
bool a_moved_subscription_keeps_its_link() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Recorder recorder;
    appfw::Observable<int>::Subscription outer;
    {
        auto inner = observable.subscribe(&Recorder::observe, &recorder);
        outer = std::move(inner);
        expect(!inner.active(), "the moved-from subscription is inert");
    }
    bool ok = expect(outer.active(), "the moved-to subscription owns the link");
    ok = expect(observable.observer_count() == 1, "and it is still registered exactly once") && ok;
    observable.set(1);
    scheduler.flush();
    ok = expect(recorder.calls == 1, "the transferred link still delivers") && ok;
    return ok;
}

// A subscription may outlive its observable (a window torn down before its service): the observable
// detaches its nodes, so the later release is a no-op instead of a dangling unlink.
bool a_subscription_may_outlive_its_observable() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int>::Subscription survivor;
    {
        appfw::Observable<int> observable(scheduler, 0);
        Recorder recorder;
        survivor = observable.subscribe(&Recorder::observe, &recorder);
        expect(survivor.active(), "the link is live while the observable is");
    }
    // The observable is gone and the recorder with it, so nothing may be called; the release must
    // simply be safe.
    bool ok = expect(survivor.active(), "the subscription object still owns its (detached) node");
    survivor.unsubscribe();
    ok = expect(!survivor.active(), "releasing a detached node is a safe no-op") && ok;
    ok = expect(!scheduler.pending(), "and it did not touch the scheduler") && ok;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Coalescing through an UpdateTicket: ADR-050 section 6's form, where the observer is a component.
// ---------------------------------------------------------------------------------------------

bool a_ticket_observer_updates_once_per_tick() {
    struct Component {
        ui::UpdateTicket ticket;
        int updates = 0;
    };
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    Component component;
    component.ticket.context = &component;
    component.ticket.update = [](void* context) { ++static_cast<Component*>(context)->updates; };
    scheduler.attach(component.ticket);
    auto subscription = observable.subscribe(component.ticket);

    for (int i = 1; i <= 50; ++i) {
        observable.set(i);
    }
    scheduler.flush();
    bool ok = expect(component.updates == 1,
                     "50 sets against a ticket observer ran its update once: got " +
                         std::to_string(component.updates));
    ok = expect(observable.get() == 50 && component.ticket.attached(),
                "the observer stays attached and reads the final value through the observable") &&
         ok;
    observable.set(51);
    scheduler.flush();
    ok = expect(component.updates == 2, "the next change schedules one more update") && ok;
    (void)subscription;
    return ok;
}

// An observer that writes during a notification is delivered in a later pass, bounded by the
// scheduler's pass budget: never an unbounded loop inside one flush.
bool an_observer_writing_again_is_bounded_by_the_pass_budget() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    const auto bump = [](void* context, const int& value) {
        static_cast<appfw::Observable<int>*>(context)->set(value + 1);
    };
    auto subscription = observable.subscribe(bump, &observable);

    observable.set(1);
    const ui::UpdateFlushStats stats = scheduler.flush();
    bool ok = expect(stats.passes <= ui::UpdateScheduler::kDefaultMaxPasses,
                     "the pass budget bounds a self-triggering observer: passes=" +
                         std::to_string(stats.passes));
    ok = expect(stats.updated <= ui::UpdateScheduler::kDefaultMaxPasses,
                "notifications per flush are bounded too: updated=" +
                    std::to_string(stats.updated)) &&
         ok;
    ok = expect(observable.get() == static_cast<int>(stats.updated) + 1,
                "each delivered notification advanced the value exactly once: value=" +
                    std::to_string(observable.get())) &&
         ok;
    ok = expect(stats.pass_limit_hit, "the loop is reported as a limit hit rather than spun") && ok;
    (void)subscription;
    return ok;
}

// Two observables on one scheduler keep their own dirty bits: a flush delivers only the one that
// changed, and an unchanged `set()` dirties nothing.
bool two_observables_coalesce_independently() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> temperature(scheduler, 20);
    appfw::Observable<int> humidity(scheduler, 40);
    Recorder temperature_observer;
    Recorder humidity_observer;
    auto first = temperature.subscribe(&Recorder::observe, &temperature_observer);
    auto second = humidity.subscribe(&Recorder::observe, &humidity_observer);

    temperature.set(21);
    temperature.set(22);
    scheduler.flush();
    bool ok = expect(temperature_observer.calls == 1 && humidity_observer.calls == 0,
                     "only the observable that changed notified");

    humidity.set(41);
    temperature.set(22); // unchanged: does not dirty
    scheduler.flush();
    ok = expect(temperature_observer.calls == 1, "an unchanged set did not notify again") && ok;
    ok = expect(humidity_observer.calls == 1, "the other observable notified on its own change") &&
         ok;
    (void)first;
    (void)second;
    return ok;
}

// The type surface the design promises: an observable is neither copyable nor movable (its ticket
// address is part of its identity), and constructing one attaches exactly one ticket.
bool the_type_surface_is_stable() {
    static_assert(!std::is_copy_constructible_v<appfw::Observable<int>>);
    static_assert(!std::is_copy_assignable_v<appfw::Observable<int>>);
    static_assert(!std::is_move_constructible_v<appfw::Observable<int>>);
    static_assert(!std::is_move_assignable_v<appfw::Observable<int>>);
    static_assert(std::is_nothrow_move_constructible_v<appfw::Observable<int>::Subscription>);

    ui::UpdateScheduler scheduler;
    const std::size_t before = scheduler.attached_count();
    appfw::Observable<int> observable(scheduler, 0);
    bool ok = expect(scheduler.attached_count() == before + 1, "constructing attaches one ticket");
    ok = expect(!scheduler.pending(), "and does not schedule anything on its own") && ok;
    return ok;
}

} // namespace

bool subscription_semantics() {
    bool ok = delivery_is_a_reference_to_the_live_value();
    ok = self_unsubscribe_stops_after_the_current_call() && ok;
    ok = unsubscribe_of_a_later_observer_takes_effect_in_the_same_pass() && ok;
    ok = unsubscribe_outside_a_flush_removes_immediately() && ok;
    ok = a_moved_subscription_keeps_its_link() && ok;
    ok = a_subscription_may_outlive_its_observable() && ok;
    ok = a_ticket_observer_updates_once_per_tick() && ok;
    ok = an_observer_writing_again_is_bounded_by_the_pass_budget() && ok;
    ok = two_observables_coalesce_independently() && ok;
    ok = the_type_surface_is_stable() && ok;
    return ok;
}

} // namespace appfw_test
