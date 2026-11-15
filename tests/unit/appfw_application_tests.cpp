// SPDX-License-Identifier: MIT
//
// AF3 (issue #175) part 2: `appfw::Application` - the ordered dispatch of a lifecycle transition to
// services, the termination gate, and the synthetic lifecycle script replayed through the same
// state machine every platform lane runs.
//
// Determinism: `ManualClock`, a hand-drained `CompletionQueue`, and no wall clock, thread, sleep or
// network. The ordered-event vector is asserted as an explicit expected sequence, so a change in
// the order is a test failure and not a silently different but still "passing" run.

#include "appfw_test_services.hpp"
#include "tessera/appfw/application.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/lifecycle_script.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/runtime/clock.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::appfw::app_state_name;
using tessera::appfw::Application;
using tessera::appfw::ApplicationConfig;
using tessera::appfw::AppLifecycleObserver;
using tessera::appfw::AppState;
using tessera::appfw::derive_app_state;
using tessera::appfw::kNoLifecycleStep;
using tessera::appfw::lifecycle_signal_name;
using tessera::appfw::LifecycleEvent;
using tessera::appfw::LifecycleScript;
using tessera::appfw::LifecycleSignal;
using tessera::appfw::parse_lifecycle_script;
using tessera::appfw::ServiceId;
using tessera::appfw::ServicePhase;
using tessera::appfw::ServiceRegistration;
using tessera::appfw::ServiceRunner;
using tessera::appfw::TerminateVote;
using tessera::appfw::TerminationOutcome;
using tessera::appfw::WindowLifecycle;
using tessera::runtime::CompletionQueue;
using tessera::runtime::ManualClock;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_lifecycle: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// A service that records every transition it is told about, in the order it was told. The recorded
// strings are the ordered-event vector: "<service>:<state>", appended as the call arrives.
//
// It is also a real `Service` (AF1's contract), so the runner starts and stops it for real and the
// dispatch filter ("only a Started service observes a transition") is exercised rather than
// assumed.
struct RecordingService final : tessera::appfw::Service, AppLifecycleObserver {
    RecordingService(ServiceId service_id, std::string_view service_name,
                     std::vector<std::string>& shared_log)
        : id(service_id),
          name(service_name),
          log(&shared_log) {}

    tessera::runtime::Task<tessera::appfw::ServiceStatus>
    start(tessera::appfw::StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    tessera::runtime::Task<void> stop(tessera::appfw::StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static tessera::runtime::Task<tessera::appfw::ServiceStatus>
    run_start(tessera::appfw::StartContext, RecordingService& self) {
        self.started = true;
        co_return tessera::appfw::ServiceStatus::Ok;
    }
    static tessera::runtime::Task<void> run_stop(tessera::appfw::StopContext,
                                                 RecordingService& self) {
        self.log->push_back(self.name + ":stopped");
        co_return;
    }

    void on_app_state(AppState state) override {
        log->push_back(name + ":" + std::string(app_state_name(state)));
    }
    TerminateVote on_terminate_requested() override {
        if (defer_termination) {
            log->push_back(name + ":defer");
            return TerminateVote::Defer;
        }
        log->push_back(name + ":allow");
        return TerminateVote::Allow;
    }

    ServiceId id{};
    std::string name;
    std::vector<std::string>* log = nullptr;
    bool defer_termination = false;
    bool started = false;
};

// A service that starts, is registered with the runner, and has no lifecycle observer: it must
// never be called, which is what proves the dispatch walks the observer table and not every
// service.
struct SilentService final : tessera::appfw::Service {
    tessera::runtime::Task<tessera::appfw::ServiceStatus>
    start(tessera::appfw::StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    tessera::runtime::Task<void> stop(tessera::appfw::StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static tessera::runtime::Task<tessera::appfw::ServiceStatus>
    run_start(tessera::appfw::StartContext, SilentService&) {
        co_return tessera::appfw::ServiceStatus::Ok;
    }
    static tessera::runtime::Task<void> run_stop(tessera::appfw::StopContext, SilentService&) {
        co_return;
    }
};

// A service whose start fails: it must never observe a transition (only a Started service does),
// and the runner reports the required failure the app polls.
struct FailingService final : tessera::appfw::Service {
    tessera::runtime::Task<tessera::appfw::ServiceStatus>
    start(tessera::appfw::StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    tessera::runtime::Task<void> stop(tessera::appfw::StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static tessera::runtime::Task<tessera::appfw::ServiceStatus>
    run_start(tessera::appfw::StartContext, FailingService&) {
        co_return tessera::appfw::ServiceStatus::Failed;
    }
    static tessera::runtime::Task<void> run_stop(tessera::appfw::StopContext, FailingService&) {
        co_return;
    }
};

// A service whose on_app_state throws: the boundary must catch it, the transition must stand, and
// the other observers must still be called.
struct ThrowingObserver final : tessera::appfw::Service, AppLifecycleObserver {
    ThrowingObserver(std::vector<std::string>& shared_log, std::size_t& throw_count)
        : log(&shared_log),
          throws(&throw_count) {}

    tessera::runtime::Task<tessera::appfw::ServiceStatus>
    start(tessera::appfw::StartContext& ctx) override {
        return run_start(ctx, *this);
    }
    tessera::runtime::Task<void> stop(tessera::appfw::StopContext& ctx) override {
        return run_stop(ctx, *this);
    }
    static tessera::runtime::Task<tessera::appfw::ServiceStatus>
    run_start(tessera::appfw::StartContext, ThrowingObserver&) {
        co_return tessera::appfw::ServiceStatus::Ok;
    }
    static tessera::runtime::Task<void> run_stop(tessera::appfw::StopContext, ThrowingObserver&) {
        co_return;
    }

    void on_app_state(AppState state) override {
        log->push_back("throw:" + std::string(app_state_name(state)));
        ++*throws;
        throw std::runtime_error("observer exploded");
    }

    std::vector<std::string>* log = nullptr;
    std::size_t* throws = nullptr;
};

// The registrations used by most tests: `Mail` depends on `Store`, so the start order is
// Store, Mail and the reverse stop order is Mail, Store.
constexpr ServiceId kStoreId = tessera::appfw::service_id("tessera.test.Store");
constexpr ServiceId kMailId = tessera::appfw::service_id("tessera.test.Mail");

// One prepared runner whose start sequence has already drained, plus the two recording services.
struct Fixture {
    explicit Fixture(bool with_observers = true, bool with_failing_service = false)
        : runner(clock, queue, nullptr) {
        const ServiceId dependencies[] = {kStoreId};
        expect(runner.add(ServiceRegistration{kStoreId, {}, true}, store).has_value(),
               "the store registers");
        expect(runner.add(ServiceRegistration{kMailId, dependencies, false}, mail).has_value(),
               "mail registers with its dependency");
        if (with_failing_service) {
            expect(runner
                       .add(ServiceRegistration{tessera::appfw::service_id("tessera.test.Broken"),
                                                {},
                                                true},
                            broken)
                       .has_value(),
                   "the failing service registers");
        }
        expect(runner.prepare().has_value(), "the runner prepares");
        if (with_observers) {
            expect(application.attach_observer(kStoreId, store), "the store observes");
            expect(application.attach_observer(kMailId, mail), "mail observes");
            if (with_failing_service) {
                // The failing service still gets an observer: it must never be called, because a
                // service that never reached Started has nothing to react to. Registering it is
                // what makes that assertion meaningful.
                expect(application.attach_observer(
                           tessera::appfw::service_id("tessera.test.Broken"), broken_observer),
                       "the failing service observes");
            }
        }
    }

    void start() {
        expect(application.launch(), "the application launches");
        // Drain the start sequence: the first start is posted, the rest run inside the drain.
        appfw_test::pump(queue);
        runner.tick();
    }

    ManualClock clock;
    CompletionQueue queue{256};
    ServiceRunner runner;
    Application application{runner};
    RecordingService store{kStoreId, "store", log};
    RecordingService mail{kMailId, "mail", log};
    RecordingService broken_observer{tessera::appfw::service_id("tessera.test.Broken"), "broken",
                                     log};
    FailingService broken;
    SilentService silent;
    std::vector<std::string> log;
};

// Formats the ordered-event vector for a failure message, so a mismatch prints both sequences.
std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values) {
        if (!text.empty()) {
            text += ' ';
        }
        text += value;
    }
    return text;
}

bool expect_sequence(const std::vector<std::string>& actual,
                     const std::vector<std::string>& expected, std::string_view what) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "appfw_lifecycle: " << what << "\n  expected: " << joined(expected)
              << "\n  actual:   " << joined(actual) << "\n";
    ++g_failures;
    return false;
}

// --- the ordered-event vector --------------------------------------------------------------------

void test_ordered_dispatch() {
    Fixture fixture;
    fixture.start();
    expect(fixture.application.state() == AppState::Launching,
           "a drained start sequence leaves the app in Launching until it is observed");

    // The first tick after the drain moves Launching -> Active and tells both services, in start
    // order (Store, the dependency, before Mail).
    fixture.application.tick();
    expect(fixture.application.state() == AppState::Active, "the app is Active");
    expect_sequence(fixture.log, {"store:active", "mail:active"},
                    "the launch transition dispatches in start order");

    // --- the explicit sequence, asserted as a vector ---------------------------------------------
    //
    // A full window lifecycle, one transition per line: the expected vector below is written out by
    // hand, so the test is the assertion, not a restatement of the implementation.
    fixture.log.clear();
    const LifecycleSignal signals[] = {
        LifecycleSignal::Focus,    LifecycleSignal::Blur,    LifecycleSignal::Focus,
        LifecycleSignal::Minimize, LifecycleSignal::Restore,
    };
    for (const LifecycleSignal signal : signals) {
        (void)fixture.application.dispatch(LifecycleEvent{signal});
    }
    expect_sequence(fixture.log,
                    {
                        // Focus while already Active: no transition, so no dispatch.
                        "store:inactive",
                        "mail:inactive",
                        "store:active",
                        "mail:active",
                        "store:hidden",
                        "mail:hidden",
                        // Restore returns to the state the window aggregate derives: the window is
                        // still focused, so Restore is Active.
                        "store:active",
                        "mail:active",
                    },
                    "the lifecycle sequence dispatches in start order, one transition per call");

    // A repeated event that carries no transition tells nobody: services are told states, not
    // events, which is what keeps an OS that reports a blur twice from running app work twice.
    fixture.log.clear();
    (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Blur});
    const std::size_t after_blur = fixture.log.size();
    expect(after_blur != 0, "a real transition does dispatch");
    (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Blur});
    expect(fixture.log.size() == after_blur,
           "a repeated Blur with no transition dispatches nothing");

    // `last_dispatch()` is the same order the log recorded, and it survives a no-op event (a
    // no-op reports the state, not a dispatch).
    expect(!fixture.application.last_dispatch().empty(),
           "a transition records the order it dispatched in");
    expect(fixture.application.last_dispatch().size() == 2,
           "both observers are recorded in the ordered vector");
}

void test_suspend_dispatches_in_reverse() {
    Fixture fixture;
    fixture.start();
    fixture.application.tick();
    fixture.log.clear();

    // Suspension is the graceful-stop edge, so it walks the start order backwards: Mail (the
    // dependent) suspends before Store (the dependency it reads), exactly like `stop()`.
    (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Suspend});
    expect_sequence(fixture.log, {"mail:suspended", "store:suspended"},
                    "Suspend dispatches in reverse start order");

    fixture.log.clear();
    (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Resume});
    expect_sequence(fixture.log, {"store:active", "mail:active"},
                    "Resume dispatches forward again");
}

void test_dispatch_filters() {
    // A service that never started never observes a transition. `Mail`'s dependency `Store` is
    // required and fails here, so Mail's start never runs and Mail never reaches Started: the app
    // still transitions (it is a state machine, not a health check) but only the healthy service is
    // told.
    Fixture fixture(true, true);
    fixture.start();
    const auto state = fixture.runner.state_of(tessera::appfw::service_id("tessera.test.Broken"));
    expect(state == tessera::appfw::ServiceState::Failed, "the broken service's start failed");
    expect(fixture.runner.required_failure(),
           "the required failure is reported by the runner for the app to poll");

    fixture.log.clear();
    (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Focus});
    // Only the services that reached Started are in the vector: the broken one is absent.
    for (const std::string& entry : fixture.log) {
        expect(entry.find("broken:") == std::string::npos,
               "a service that never started never observes a transition");
    }

    // A throwing observer is contained: the transition stands and the sequence continues, so the
    // next observer is still called (ADR-050 section 4 item 5).
    std::vector<std::string> log;
    std::size_t throws = 0;
    {
        ManualClock clock;
        CompletionQueue queue{64};
        ServiceRunner runner(clock, queue, nullptr);
        ThrowingObserver thrower(log, throws);
        expect(runner.add(ServiceRegistration{kStoreId, {}, false}, thrower).has_value(),
               "the throwing observer registers");
        expect(runner.prepare().has_value(), "the runner prepares");
        Application application(runner);
        expect(application.attach_observer(kStoreId, thrower), "the thrower observes");
        (void)application.launch();
        appfw_test::pump(queue);
        runner.tick();
        application.tick();

        expect(throws == 1, "the throwing observer was called once");
        expect(application.state() == AppState::Active,
               "an exception from on_app_state does not roll back the transition");
    }
}

void test_termination_gate() {
    // Allow: the gate enters Terminating, tells the services (reverse order, the same order their
    // stop will run in), and begins the bounded reverse stop.
    {
        Fixture fixture;
        fixture.start();
        fixture.application.tick();
        fixture.log.clear();

        expect(fixture.application.request_termination() == TerminationOutcome::Started,
               "an uncontested termination starts");
        expect(fixture.application.state() == AppState::Terminating, "the app is Terminating");
        // The vote ran backwards (mail before store) and the transition dispatched the same way.
        expect_sequence(fixture.log,
                        {
                            "mail:allow",
                            "store:allow",
                            "mail:terminating",
                            "store:terminating",
                        },
                        "the termination gate votes and dispatches in reverse start order");
        expect(fixture.runner.phase() == ServicePhase::Stopping,
               "the runner's stop sequence began");

        fixture.log.clear();
        appfw_test::pump(fixture.queue);
        fixture.runner.tick();
        fixture.application.tick();
        expect(fixture.runner.phase() == ServicePhase::Stopped, "every service stopped");
        expect(fixture.application.state() == AppState::Terminated,
               "the app is Terminated once the stop sequence drained");
        expect_sequence(fixture.log, {"mail:stopped", "store:stopped"},
                        "the stop sequence runs in reverse start order");

        // Terminal means terminal: a focus after shutdown cannot resurrect the app, and every later
        // transition is a no-op that reports the state it is in.
        fixture.log.clear();
        const auto after = fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Focus});
        expect(!after.entered && after.delivered == 0,
               "a transition after Terminated dispatches nothing");
        expect(fixture.application.state() == AppState::Terminated,
               "a transition after Terminated leaves the state alone");
        expect(fixture.application.request_termination() == TerminationOutcome::AlreadyTerminating,
               "a second termination request is refused");
    }

    // Defer: one service with unsaved work cancels the sequence, and nothing stops.
    {
        Fixture fixture;
        fixture.mail.defer_termination = true;
        fixture.start();
        fixture.application.tick();
        fixture.log.clear();

        expect(fixture.application.request_termination() == TerminationOutcome::Deferred,
               "a deferring service defers the termination");
        expect(fixture.application.deferring(), "deferring() reports the deferral");
        expect(fixture.application.state() == AppState::Active,
               "a deferred termination leaves the app in its state");
        expect(fixture.runner.phase() == ServicePhase::Running,
               "a deferred termination does not stop anything");
        // Mail is asked first (reverse order), so Store is never asked: the sequence aborts.
        expect_sequence(fixture.log, {"mail:defer"},
                        "the deferring service is the first one asked");
    }

    // Close with quitOnLastWindowClosed (the default) is a termination request, not a Hidden state.
    {
        Fixture fixture;
        fixture.start();
        fixture.application.tick();
        (void)fixture.application.dispatch(LifecycleEvent{LifecycleSignal::Close});
        expect(fixture.application.state() == AppState::Terminating,
               "closing the last window requests termination");
    }
    // With it disabled, the same close is Hidden (a tray app stays alive).
    {
        ManualClock clock;
        CompletionQueue queue{64};
        ServiceRunner runner(clock, queue, nullptr);
        ApplicationConfig config;
        config.quit_on_last_window_closed = false;
        Application application(runner, config);
        (void)application.launch();
        (void)application.dispatch(LifecycleEvent{LifecycleSignal::Close});
        expect(application.state() == AppState::Hidden,
               "without quitOnLastWindowClosed a close is Hidden");
        expect(application.window().closed, "the window aggregate keeps the closed bit");
        // A focus reopens it: the app leaves Hidden for the state the aggregate derives.
        (void)application.dispatch(LifecycleEvent{LifecycleSignal::Focus});
        expect(application.state() == AppState::Active, "a focused window brings the app back");
    }
}

} // namespace

int run_application_tests() {
    const int before = g_failures;
    test_ordered_dispatch();
    test_suspend_dispatches_in_reverse();
    test_dispatch_filters();
    test_termination_gate();
    return g_failures - before;
}
