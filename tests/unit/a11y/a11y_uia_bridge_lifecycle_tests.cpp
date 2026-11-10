// SPDX-License-Identifier: MIT
//
// Bridge lifecycle: the worker owns the apartment, initialization failures are values, `stop()`
// joins in the right order, and a client call that cannot be serviced inside its bound reports
// `Timeout` instead of blocking forever.

#include "a11y_uia_fake_apartment.hpp"
#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_bridge.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::UiaBridge;
using tessera::ui::a11y::UiaBridgeConfig;
using tessera::ui::a11y::UiaBridgePhase;
using tessera::ui::a11y::UiaError;
using tessera::ui::a11y::UiaProvider;

struct Harness {
    FakeApartment* apartment = nullptr;
    std::unique_ptr<UiaBridge> bridge;
};

std::unique_ptr<Harness> make_harness() {
    auto harness = std::make_unique<Harness>();
    auto apartment = std::make_unique<FakeApartment>();
    harness->apartment = apartment.get();
    UiaBridgeConfig config;
    config.idle_wait_ms = 1000;
    config.shutdown_wait_ms = 5000;
    harness->bridge = std::make_unique<UiaBridge>(config, std::move(apartment));
    return harness;
}

bool start_join_restart() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    FakeApartment& apartment = *harness->apartment;

    if (!expect(!bridge.started(), "a fresh bridge is not started") ||
        !expect(bridge.phase() == UiaBridgePhase::Idle, "a fresh bridge is idle")) {
        return false;
    }
    if (!expect(bridge.start().has_value(), "start spawns the worker") ||
        !expect(bridge.started(), "the worker is alive") ||
        !expect(bridge.step().has_value(), "the first step initializes") ||
        !expect(bridge.running(), "the bridge is running")) {
        return false;
    }
    if (!expect(bridge.bridge_thread() != std::this_thread::get_id(),
                "the bridge thread is not the test thread") ||
        !expect_eq(static_cast<std::int64_t>(apartment.inits()), 1, "one apartment init")) {
        return false;
    }

    bridge.stop();
    const std::vector<FakeApartment::Event> events = apartment.events();
    const bool ordered = events.size() == 2 && events[0].kind == FakeApartment::EventKind::Init &&
                         events[1].kind == FakeApartment::EventKind::Shutdown &&
                         events[0].thread == events[1].thread &&
                         events[0].thread != std::this_thread::get_id();
    if (!expect(bridge.phase() == UiaBridgePhase::Stopped, "stop reports stopped") ||
        !expect(!bridge.started(), "no worker after stop") ||
        !expect(!bridge.running(), "not running after stop") ||
        !expect(ordered, "init then shutdown on the bridge thread") ||
        !expect_eq(static_cast<std::int64_t>(apartment.shutdowns()), 1, "exactly one shutdown")) {
        return false;
    }

    bridge.stop(); // idempotent
    if (!expect_eq(static_cast<std::int64_t>(apartment.shutdowns()), 1,
                   "a second stop does not shut down twice") ||
        !expect(!bridge.step().has_value(), "a step after stop reports a value") ||
        !expect(bridge.step().error() == UiaError::NotStarted, "the error is NotStarted")) {
        return false;
    }

    if (!expect(bridge.start().has_value(), "restart spawns a new worker") ||
        !expect(bridge.step().has_value(), "restart re-initializes") ||
        !expect(bridge.running(), "running again")) {
        return false;
    }
    bridge.stop();
    return expect_eq(static_cast<std::int64_t>(apartment.inits()), 2, "two inits across restart") &&
           expect_eq(static_cast<std::int64_t>(apartment.shutdowns()), 2, "two shutdowns");
}

bool init_failure_is_a_value() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    FakeApartment& apartment = *harness->apartment;
    apartment.fail_init_with(UiaError::ApartmentUnavailable);

    if (!expect(bridge.start().has_value(), "start does not block on apartment init")) {
        return false;
    }
    const auto step = bridge.step();
    if (!expect(!step.has_value(), "the failed init is reported") ||
        !expect(step.error() == UiaError::ApartmentUnavailable, "the init error is preserved") ||
        !expect(bridge.phase() == UiaBridgePhase::Failed, "the phase is Failed") ||
        !expect(!bridge.started(), "the worker exited")) {
        return false;
    }
    bridge.stop();
    if (!expect(bridge.phase() == UiaBridgePhase::Failed, "stop keeps the failure") ||
        !expect_eq(static_cast<std::int64_t>(apartment.shutdowns()), 0,
                   "a failed init is not torn down")) {
        return false;
    }

    apartment.clear_init_failure();
    const bool restarted =
        bridge.start().has_value() && bridge.step().has_value() && bridge.running();
    bridge.stop();
    return expect(restarted, "a cleared failure restarts the bridge");
}

bool client_wait_is_bounded() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    FakeApartment& apartment = *harness->apartment;
    // Block the message pump so the worker cannot finish another iteration.
    apartment.close_pump_gate();

    if (!expect(bridge.start().has_value(), "start succeeds") ||
        !expect(bridge.step().has_value(), "the first step completes before the pump blocks")) {
        apartment.open_pump_gate();
        return false;
    }

    bool ran = false;
    const auto timed_out = bridge.run_on_bridge([&](UiaProvider&) { ran = true; }, 50);
    const bool rejected = !timed_out.has_value() && timed_out.error() == UiaError::Timeout && !ran;
    apartment.open_pump_gate();

    bool later_ran = false;
    const auto later = bridge.run_on_bridge([&](UiaProvider&) { later_ran = true; }, 5000);
    bridge.stop();
    return expect(rejected, "a blocked worker reports Timeout without running the job") &&
           expect(later.has_value(), "the bridge recovers after the pump is released") &&
           expect(later_ran, "the retried job runs");
}

bool pending_snapshot_survives_stop_until_restart() {
    auto harness = make_harness();
    UiaBridge& bridge = *harness->bridge;
    if (!expect(bridge.start().has_value(), "start succeeds") ||
        !expect(bridge.step().has_value(), "step initializes")) {
        return false;
    }
    bridge.publish(button_snapshot("Orphan"));
    bridge.stop();
    if (!expect_eq(static_cast<std::int64_t>(bridge.handoff_stats().taken), 0,
                   "stop does not adopt a snapshot")) {
        return false;
    }
    std::uint64_t generation = 0;
    if (!expect(bridge.start().has_value(), "restart succeeds") ||
        !expect(bridge.step().has_value(), "restart initializes")) {
        return false;
    }
    (void)bridge.run_on_bridge([&](UiaProvider& provider) { generation = provider.generation(); });
    bridge.stop();
    return expect_eq(static_cast<std::int64_t>(generation), 1,
                     "the pending snapshot is adopted after restart");
}

} // namespace

int main() {
    if (!start_join_restart()) {
        return 1;
    }
    if (!init_failure_is_a_value()) {
        return 2;
    }
    if (!client_wait_is_bounded()) {
        return 3;
    }
    if (!pending_snapshot_survives_stop_until_restart()) {
        return 4;
    }
    std::cout << "a11y_uia_bridge_lifecycle: ok\n";
    return 0;
}
