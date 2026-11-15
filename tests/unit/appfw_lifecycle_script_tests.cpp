// SPDX-License-Identifier: MIT
//
// AF3 (issue #175) part 3: the synthetic lifecycle script.
//
// The acceptance is "a synthetic lifecycle script passes per platform", so the script is a
// repository artifact (`tests/fixtures/lifecycle/desktop.json`) and this file is the one thing that
// replays it: every platform lane that builds `appfw_lifecycle_tests` reads the same bytes and
// drives them through the same `appfw::Application`. Nothing here is platform-conditional, and
// nothing reads a wall clock, a thread or an OS callback - the script is the whole input.
//
// Two checks per step, deliberately independent:
//   1. the state the machine reached equals the step's `expect`;
//   2. the ADR-050 section 2 table, applied to the window aggregate the event left behind, produces
//      that same state - which catches a machine that arrived somewhere the table does not
//      sanction.

#include "tessera/appfw/application.hpp"
#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/lifecycle_script.hpp"
#include "tessera/appfw/service_runner.hpp"
#include "tessera/runtime/clock.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using tessera::appfw::app_state_name;
using tessera::appfw::Application;
using tessera::appfw::AppState;
using tessera::appfw::derive_app_state;
using tessera::appfw::lifecycle_signal_name;
using tessera::appfw::LifecycleScript;
using tessera::appfw::parse_lifecycle_script;
using tessera::appfw::ServiceRunner;
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

// Reads a script from the directory the target definition supplies, so the same bytes are read on
// every platform rather than a path relative to the working directory.
std::string read_script(const char* name) {
    std::string path = TESSERA_LIFECYCLE_SCRIPT_DIR;
    path += '/';
    path += name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Replays one script and checks every step. Returns the number of transitions the script drove, so
// a caller can assert the script was not vacuous.
std::size_t replay(const LifecycleScript& script) {
    ManualClock clock;
    CompletionQueue queue{64};
    ServiceRunner runner(clock, queue, nullptr);
    Application application(runner);
    // No services: this file is about the state machine, and `Fixture` in
    // appfw_application_tests.cpp covers the dispatch to services. An unprepared runner has no
    // start sequence, so the first tick leaves `Launching` immediately.
    (void)application.launch();
    application.tick();
    expect(application.state() != AppState::Launching,
           "the app leaves Launching before the script replays");

    std::size_t transitions = 0;
    for (std::size_t index = 0; index < script.steps.size(); ++index) {
        const auto& step = script.steps[index];
        const auto result = application.dispatch(step.event);
        transitions += result.entered ? 1 : 0;
        if (!expect(result.state == step.expected,
                    std::string("step ") + std::to_string(index) + " (" +
                        std::string(lifecycle_signal_name(step.event.signal)) + ") derives " +
                        std::string(app_state_name(step.expected)))) {
            std::cerr << "  observed: " << app_state_name(result.state) << "\n";
            continue;
        }
        const AppState derived = derive_app_state(application.window(), application.suspended());
        const bool terminal =
            result.state == AppState::Terminating || result.state == AppState::Terminated;
        if (!terminal) {
            expect(derived == result.state,
                   "the scripted state matches the ADR table's derivation");
        }
    }
    return transitions;
}

void test_desktop_script() {
    const std::string text = read_script("desktop.json");
    expect(!text.empty(), "the lifecycle script fixture is readable");
    if (text.empty()) {
        return;
    }
    auto script = parse_lifecycle_script(text);
    expect(script.has_value(), "the repository's lifecycle script parses");
    if (!script.has_value()) {
        std::cerr << "  detail: " << script.error().detail << "\n";
        return;
    }
    expect(!script->steps.empty(), "the script has steps");
    expect(script->steps.size() == 18, "the desktop script has the expected number of steps");

    const std::size_t transitions = replay(*script);
    expect(transitions != 0, "the script drove real transitions");
    // Which steps are transitions is part of the fixture's meaning: a script of 18 events drives 15
    // transitions, because two `focus` calls and one `blur` land in the state already in force. If
    // that number moves, either the script or the state machine changed and the fixture is stale.
    expect(transitions == 15, "the script drives the expected number of transitions");
}

// The decoder is checked against the real file too, so a fixture edit that breaks the format fails
// here rather than only in the pure decoder tests.
void test_fixture_is_well_formed() {
    const std::string text = read_script("desktop.json");
    if (text.empty()) {
        return;
    }
    auto script = parse_lifecycle_script(text);
    if (!expect(script.has_value(), "the fixture round-trips through the decoder")) {
        return;
    }
    expect(script->name == "lifecycle", "the fixture names itself");
}

// The metrics channel: a platform without a lifecycle event reports occlusion through the window
// snapshot, which must derive exactly the same states as the event channel and drive the same
// script. This is the second half of "lifecycle events reach services in order" - the events can
// arrive as either normalized window events or a metrics snapshot, and both must land in one
// machine.
void test_snapshot_channel_matches_events() {
    const std::string text = read_script("desktop.json");
    if (text.empty()) {
        return;
    }
    auto script = parse_lifecycle_script(text);
    if (!expect(script.has_value(), "the fixture parses for the snapshot channel")) {
        return;
    }

    // Replay the same script twice: once through `dispatch` and once by pushing the window state
    // each event implies through `apply_window_snapshot`. The two state vectors must be identical.
    ManualClock clock;
    CompletionQueue queue{64};
    ServiceRunner runner(clock, queue, nullptr);
    Application events(runner);
    (void)events.launch();
    events.tick();

    ManualClock mirror_clock;
    CompletionQueue mirror_queue{64};
    ServiceRunner mirror_runner(mirror_clock, mirror_queue, nullptr);
    Application snapshots(mirror_runner);
    (void)snapshots.launch();
    snapshots.tick();

    for (const auto& step : script->steps) {
        const AppState from_event = events.dispatch(step.event).state;
        // The snapshot the platform would report for the same transition, derived from the
        // aggregate the machine itself now holds, then applied to the mirror app that has walked
        // the same path through the snapshot channel.
        const AppState from_snapshot =
            snapshots
                .apply_window_snapshot(events.window().focused, events.window().occluded,
                                       events.window().minimized)
                .state;
        // Two states are not window-derived, so the snapshot channel cannot produce them: `Suspend`
        // (the OS is about to freeze the process) and the `Terminating`/`Terminated` pair (a
        // termination request, not a window state). The asymmetry is the ADR's, not a gap: a
        // snapshot describes the window, and the state machine folds it together with the freeze
        // signal and the termination gate.
        if (from_event == AppState::Suspended || from_event == AppState::Terminating ||
            from_event == AppState::Terminated) {
            continue;
        }
        // The mirror's own suspension flag is cleared by the `resume` leg of the script before this
        // comparison would see it, so the window-derived rows are directly comparable.
        expect(from_snapshot == from_event,
               "the snapshot channel derives the same state as the event channel");
    }
}

} // namespace

int run_lifecycle_script_tests() {
    const int before = g_failures;
    test_desktop_script();
    test_fixture_is_well_formed();
    test_snapshot_channel_matches_events();
    return g_failures - before;
}
