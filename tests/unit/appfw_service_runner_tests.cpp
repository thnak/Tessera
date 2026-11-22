// SPDX-License-Identifier: MIT
//
// AF1 (issue #173) part 2: the `Service` state machine under ADR-050 section 4 — registration,
// dependency ordering, ordered start, reverse-ordered stop, and the per-service and total budgets.
//
// Every transition is observed through a hand-drained `CompletionQueue` and a `ManualClock`; no
// test waits, sleeps or reads a wall clock. `tick()` is only ever called to enforce a deadline.

#include "appfw_test_services.hpp"
#include "tessera/appfw/service_id.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/runtime/clock.hpp"

#include <string>

namespace appfw_test {

using tessera::appfw::reset_service_lookup_audit;
using tessera::appfw::service_lookup_audit;
using tessera::appfw::service_state_name;
using tessera::appfw::ServiceContext;
using tessera::appfw::ServiceErrorCode;
using tessera::appfw::ServiceId;
using tessera::appfw::ServicePhase;
using tessera::appfw::ServiceRegistration;
using tessera::appfw::ServiceRunner;
using tessera::appfw::ServiceRunnerConfig;
using tessera::appfw::ServiceState;
using tessera::runtime::ManualClock;

int run_runner_tests() {
    const int before = g_failures;

    // --- registration and prepare errors ---------------------------------------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr);

        FakeService first("first");
        FakeService second("second");
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, first).has_value(),
               "a valid registration is accepted");
        const auto duplicate =
            runner.add(ServiceRegistration{Store::kServiceId, {}, false}, second);
        expect(!duplicate.has_value(), "a duplicate registration is rejected");
        expect(duplicate.error().code == ServiceErrorCode::DuplicateId,
               "the duplicate reports DuplicateId");
        expect(runner.prepare().has_value(), "a dependency-free runner prepares");
        expect(runner.prepared(), "prepared() reports the freeze");

        const auto again = runner.prepare();
        expect(!again.has_value(), "a second prepare is rejected");
        expect(again.error().code == ServiceErrorCode::AlreadyPrepared,
               "the second prepare reports AlreadyPrepared");

        FakeService late("late");
        const auto after_freeze =
            runner.add(ServiceRegistration{Mail::kServiceId, {}, false}, late);
        expect(!after_freeze.has_value(), "add after prepare is rejected");
        expect(after_freeze.error().code == ServiceErrorCode::Frozen, "late add reports Frozen");
    }

    // --- a missing dependency and a cycle are startup errors
    // --------------------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner missing(clock, queue, nullptr);
        FakeService service("service");
        const ServiceId needs_store[] = {Store::kServiceId};
        expect(missing.add(ServiceRegistration{Mail::kServiceId, needs_store, true}, service)
                   .has_value(),
               "the registration itself is valid");
        const auto prepared = missing.prepare();
        expect(!prepared.has_value(), "a missing dependency fails prepare");
        expect(prepared.error().code == ServiceErrorCode::MissingDependency,
               "the failure reports MissingDependency");
        expect(!missing.prepared(), "a failed prepare leaves the runner unprepared");
        expect(missing.start_order().empty(), "an unprepared runner has no start order");
        expect(!missing.begin_start(), "an unprepared runner cannot start");

        ServiceRunner cyclic(clock, queue, nullptr);
        FakeService a("a");
        FakeService b("b");
        const ServiceId a_needs_b[] = {Mail::kServiceId};
        const ServiceId b_needs_a[] = {Store::kServiceId};
        expect(cyclic.add(ServiceRegistration{Store::kServiceId, a_needs_b, true}, a).has_value(),
               "register the first cycle node");
        expect(cyclic.add(ServiceRegistration{Mail::kServiceId, b_needs_a, true}, b).has_value(),
               "register the second cycle node");
        const auto cycle = cyclic.prepare();
        expect(!cycle.has_value(), "a cycle fails prepare");
        expect(cycle.error().code == ServiceErrorCode::DependencyCycle,
               "the failure reports DependencyCycle");
    }

    // --- ordered start, reverse-ordered stop
    // ------------------------------------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr);

        FakeService store("store");
        FakeService mail("mail");
        FakeService ui("ui");
        // Registration order is deliberately not dependency order: ui needs store, store is second.
        const ServiceId ui_needs_store[] = {Store::kServiceId};
        expect(runner.add(ServiceRegistration{Mail::kServiceId, {}, true}, mail).has_value(),
               "register mail");
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, store).has_value(),
               "register store");
        expect(runner.add(ServiceRegistration{UnknownId::kServiceId, ui_needs_store, true}, ui)
                   .has_value(),
               "register ui");
        expect(runner.prepare().has_value(), "the dependency graph prepares");

        const auto order = runner.start_order();
        expect(order.size() == 3, "the start order covers every service");
        expect(order.size() == 3 && order[0] == Mail::kServiceId,
               "an independent service ordered by registration comes first");
        expect(order.size() == 3 && order[1] == Store::kServiceId, "then the dependency");
        expect(order.size() == 3 && order[2] == UnknownId::kServiceId, "the dependent is last");

        expect(runner.begin_start(), "begin_start accepts a prepared runner");
        expect(runner.phase() == ServicePhase::Starting, "the runner is starting");
        expect(mail.events.empty(), "begin_start does not run the first start inline");
        pump(queue);
        expect(runner.phase() == ServicePhase::Running, "a drained sequence reaches Running");
        expect(runner.stats().started == 3, "every service started");
        expect(mail.events.size() == 1 && mail.events[0] == "start:mail", "mail started first");
        expect(store.events.size() == 1 && store.events[0] == "start:store", "store started next");
        expect(ui.events.size() == 1 && ui.events[0] == "start:ui", "ui started last");
        expect(runner.state_of(Mail::kServiceId) == ServiceState::Started, "mail is Started");
        expect(!runner.sequence_active(), "the sequence is idle once Running");
        expect(!runner.begin_start(), "a second begin_start is refused");

        expect(runner.begin_stop(), "begin_stop accepts a running runner");
        pump(queue);
        expect(runner.phase() == ServicePhase::Stopped, "a drained stop reaches Stopped");
        expect(runner.stats().stopped == 3, "every service stopped");
        expect(ui.events.size() == 2 && ui.events[1] == "stop:ui", "the dependent stops first");
        expect(store.events.size() == 2 && store.events[1] == "stop:store", "then store");
        expect(mail.events.size() == 2 && mail.events[1] == "stop:mail", "then mail");
        expect(runner.state_of(Mail::kServiceId) == ServiceState::Stopped, "mail is Stopped");
        expect(!runner.begin_stop(), "a stopped runner cannot stop again");
    }

    // --- a start() may resolve its own dependency without counting as in-frame
    // ---------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceContext scope;
        FakeDependency dependency;
        expect(scope.provide<FakeDependency>(dependency).has_value(), "provide the dependency");
        expect(scope.freeze().has_value(), "freeze the scope");

        reset_service_lookup_audit();
        ServiceRunner runner(clock, queue, &scope);
        FakeService consumer("consumer");
        consumer.resolve_dependency = true;
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, consumer).has_value(),
               "register the resolving service");
        expect(runner.prepare().has_value(), "prepare it");
        expect(runner.begin_start(), "start it");
        pump(queue);
        expect(consumer.resolved == &dependency, "the start() resolved its dependency");
        expect(service_lookup_audit().in_frame == 0,
               "a start() lookup outside a FrameScope is not counted as in-frame");
        reset_service_lookup_audit();
    }

    // --- a failed start fails its dependents and fails a required runner
    // ---------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr);

        FakeService root("root");
        FakeService dependent("dependent");
        FakeService independent("independent");
        root.fail_start = true;
        const ServiceId needs_root[] = {Store::kServiceId};
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, root).has_value(),
               "register the failing required service");
        expect(runner.add(ServiceRegistration{Mail::kServiceId, needs_root, true}, dependent)
                   .has_value(),
               "register its dependent");
        expect(runner.add(ServiceRegistration{UnknownId::kServiceId, {}, false}, independent)
                   .has_value(),
               "register an optional independent service");
        expect(runner.prepare().has_value(), "prepare the failing graph");

        expect(runner.begin_start(), "start the failing graph");
        pump(queue);
        expect(runner.phase() == ServicePhase::Failed, "a required failure fails the sequence");
        expect(runner.required_failure(), "required_failure reports it");
        expect(runner.state_of(Store::kServiceId) == ServiceState::Failed, "the root is Failed");
        expect(runner.state_of(Mail::kServiceId) == ServiceState::Failed,
               "a dependent of a failed service is Failed without running");
        expect(dependent.events.empty(), "the dependent never ran");
        expect(runner.state_of(UnknownId::kServiceId) == ServiceState::Started,
               "an independent service still starts");

        expect(runner.begin_stop(), "a failed sequence can still shut down");
        pump(queue);
        expect(runner.phase() == ServicePhase::Stopped, "shutdown completes");
        expect(root.events.size() == 1, "a failed start is not stopped");
        expect(dependent.events.empty(), "a skipped service is not stopped");
        expect(independent.events.size() == 2 && independent.events[1] == "stop:independent",
               "the started service is stopped");
    }

    // --- a throwing start is converted to Failed at the service boundary
    // ---------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr);
        ThrowingService thrower;
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, false}, thrower).has_value(),
               "register a throwing service");
        expect(runner.prepare().has_value(), "prepare the throwing service");
        expect(runner.begin_start(), "start the throwing service");
        pump(queue);
        expect(thrower.entered, "the throwing service ran");
        expect(runner.state_of(Store::kServiceId) == ServiceState::Failed,
               "an exception escaping start becomes Failed");
        expect(runner.phase() == ServicePhase::Running,
               "an optional failure still leaves the runner Running");
        expect(!runner.required_failure(), "an optional failure is not a required failure");
    }

    // --- without a start deadline a hanging start stays Starting (AF1's documented default)
    // --------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr); // start deadlines are 0 = disabled
        HangingStartService hung;
        FakeService after("after");
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, hung).has_value(),
               "register the hanging service");
        expect(runner.add(ServiceRegistration{Mail::kServiceId, {}, true}, after).has_value(),
               "register the following service");
        expect(runner.prepare().has_value(), "prepare the graph");
        expect(runner.begin_start(), "start the graph");
        pump(queue);
        expect(hung.start_entered, "the hanging service started");
        expect(runner.state_of(Store::kServiceId) == ServiceState::Starting,
               "without a start deadline a hanging start stays Starting");
        expect(after.events.empty(), "the sequence cannot reach a later service while one hangs");

        // Enough simulated time to prove no deadline is armed.
        clock.advance(1000.0);
        runner.tick();
        pump(queue);
        expect(runner.state_of(Store::kServiceId) == ServiceState::Starting,
               "advancing time does not fail a start with no configured deadline");
    }

    // --- a configured start deadline fails the start and unblocks the sequence
    // ---------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunnerConfig config;
        // Not the default: AF1 ships start without a deadline, but the mechanism is implemented and
        // must work when a future ADR sets one.
        config.per_service_start_deadline_seconds = 0.5;
        ServiceRunner runner(clock, queue, nullptr, config);

        HangingStartService hung;
        FakeService after("after");
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, hung).has_value(),
               "register the hanging service");
        expect(runner.add(ServiceRegistration{Mail::kServiceId, {}, true}, after).has_value(),
               "register the following service");
        expect(runner.prepare().has_value(), "prepare the graph");
        expect(runner.begin_start(), "start the graph");
        pump(queue);
        expect(hung.start_entered, "the hanging service started");

        clock.advance(0.6);
        runner.tick();
        pump(queue);
        expect(runner.state_of(Store::kServiceId) == ServiceState::Failed,
               "a start past its deadline is Failed");
        expect(runner.stats().failed == 1, "the overrun is counted as a failure");
        expect(runner.state_of(Mail::kServiceId) == ServiceState::Started,
               "the sequence continues past the abandoned start");
        expect(runner.phase() == ServicePhase::Failed, "a required failure fails the sequence");

        expect(runner.begin_stop(), "the sequence can still shut down");
        pump(queue);
        expect(runner.phase() == ServicePhase::Stopped, "shutdown completes");
        expect(!hung.stop_entered, "a service that never reached Started is never stopped");
        expect(after.events.size() == 2 && after.events[1] == "stop:after",
               "the service that did start is stopped");
    }

    // --- the stop deadline abandons a hanging stop and shutdown continues
    // --------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunnerConfig config;
        config.per_service_stop_deadline_seconds = 0.5;
        ServiceRunner runner(clock, queue, nullptr, config);

        HangingStopService hung;
        FakeService fast("fast");
        // Reverse order: `fast` is registered second, so it stops first without hanging; `hung`
        // then hangs and must be abandoned for shutdown to finish.
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, hung).has_value(),
               "register the hanging-stop service");
        expect(runner.add(ServiceRegistration{Mail::kServiceId, {}, true}, fast).has_value(),
               "register the fast service");
        expect(runner.prepare().has_value(), "prepare the graph");
        expect(runner.begin_start(), "start the graph");
        pump(queue);
        expect(fast.events.size() == 1 && fast.events[0] == "start:fast",
               "the fast service started");

        expect(runner.begin_stop(), "begin the stop");
        pump(queue);
        expect(fast.events.size() == 2 && fast.events[1] == "stop:fast",
               "the fast service stopped first");
        expect(hung.stop_entered, "then the hanging service's stop began");
        expect(runner.state_of(Store::kServiceId) == ServiceState::Stopping,
               "the hanging stop is in flight");

        clock.advance(0.6);
        runner.tick();
        expect(runner.state_of(Store::kServiceId) == ServiceState::Abandoned,
               "a stop past its deadline is Abandoned");
        expect(runner.stats().abandoned == 1, "the abandonment is counted");
        expect(runner.phase() == ServicePhase::Stopped, "shutdown continues to completion");
    }

    // --- the total stop budget caps the whole sequence
    // ----------------------------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunnerConfig config;
        config.per_service_stop_deadline_seconds = 0.0; // no per-service cap: total does the work
        config.total_stop_deadline_seconds = 1.5;
        ServiceRunner runner(clock, queue, nullptr, config);

        FakeService first("first");
        FakeService second("second");
        first.stop_hops = 1000;
        second.stop_hops = 1000;
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, first).has_value(),
               "register the first slow stopper");
        expect(runner.add(ServiceRegistration{Mail::kServiceId, {}, true}, second).has_value(),
               "register the second slow stopper");
        expect(runner.prepare().has_value(), "prepare the slow stoppers");
        expect(runner.begin_start(), "start them");
        pump(queue);

        expect(runner.begin_stop(), "begin the total-budget stop");
        pump(queue, 1);
        clock.advance(1.6); // past the 1.5 s total budget
        runner.tick();
        expect(runner.stats().abandoned == 2,
               "the in-flight stop and every one it leaves behind are abandoned together");
        expect(runner.state_of(Store::kServiceId) == ServiceState::Abandoned,
               "a stop that never got to run inside the budget is Abandoned, not silently skipped");
        expect(runner.state_of(Mail::kServiceId) == ServiceState::Abandoned,
               "the in-flight stop is Abandoned by the total budget");
        expect(runner.phase() == ServicePhase::Stopped, "the sequence ends after the total budget");
        expect(!runner.sequence_active(), "nothing is left in flight after the budget expires");
        // The abandoned stop is suspended on a queue hop, not parked forever, so it can still
        // resume: draining lets it observe its cancelled token and unwind, which is what `abandon`
        // documents as happening at the next resume. Leaving that resume pending would destroy the
        // queue with the frame still inside the abandoned pair's cycle.
        pump(queue);
    }

    // --- tick() never runs service code
    // -------------------------------------------------------------
    {
        ManualClock clock;
        CompletionQueue queue(64);
        ServiceRunner runner(clock, queue, nullptr);
        FakeService service("service");
        expect(runner.add(ServiceRegistration{Store::kServiceId, {}, true}, service).has_value(),
               "register a cooperative service");
        expect(runner.prepare().has_value(), "prepare it");
        expect(runner.begin_start(), "start it");
        expect(service.events.empty(), "begin_start queues the first resume without running it");
        clock.advance(100.0);
        runner.tick();
        expect(service.events.empty(), "tick never runs service code");
        pump(queue);
        expect(service.events.size() == 1, "the queue drain ran the service");
    }

    // --- the state and error names are stable, since diagnostics print them
    // ------------------------
    {
        expect(std::string(service_state_name(ServiceState::Started)) == "started",
               "the Started name is stable");
        expect(std::string(service_state_name(ServiceState::Abandoned)) == "abandoned",
               "the Abandoned name is stable");
        expect(std::string(tessera::appfw::service_error_code_name(
                   ServiceErrorCode::DuplicateId)) == "duplicate id",
               "the DuplicateId name is stable");
    }

    return g_failures - before;
}

} // namespace appfw_test
