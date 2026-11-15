// SPDX-License-Identifier: MIT
//
// AF3 (issue #175) part 1: the app lifecycle state table of ADR-050 section 2, row by row, plus the
// script decoder. Pure functions only, so this file proves the *derivation* (including the rows the
// scripted sequence never visits) without a service, a queue or a clock.

#include "tessera/appfw/lifecycle.hpp"
#include "tessera/appfw/lifecycle_script.hpp"
#include "tessera/hal/window.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::appfw::app_state_from_name;
using tessera::appfw::app_state_name;
using tessera::appfw::AppState;
using tessera::appfw::derive_app_state;
using tessera::appfw::fold_window_lifecycle;
using tessera::appfw::lifecycle_signal_for;
using tessera::appfw::lifecycle_signal_from_name;
using tessera::appfw::lifecycle_signal_name;
using tessera::appfw::LifecycleEvent;
using tessera::appfw::LifecycleScriptErrorCode;
using tessera::appfw::LifecycleSignal;
using tessera::appfw::parse_lifecycle_script;
using tessera::appfw::WindowLifecycle;

int g_failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_lifecycle: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// The aggregate after a sequence of signals, starting from a focused visible window.
WindowLifecycle after(std::initializer_list<LifecycleSignal> signals) {
    WindowLifecycle window;
    for (const LifecycleSignal signal : signals) {
        window = fold_window_lifecycle(window, signal);
    }
    return window;
}

void test_state_table() {
    // The ADR's four derivation rows, each one asserted directly: a visible+focused window is
    // Active, a visible+unfocused one Inactive, anything not visible Hidden, and a frozen process
    // Suspended regardless of the window.
    expect(derive_app_state(WindowLifecycle{true, true, false, false, false}, false) ==
               AppState::Active,
           "a visible focused window is Active");
    expect(derive_app_state(WindowLifecycle{true, false, false, false, false}, false) ==
               AppState::Inactive,
           "a visible unfocused window is Inactive");
    expect(derive_app_state(WindowLifecycle{true, true, true, false, false}, false) ==
               AppState::Hidden,
           "a minimized window is Hidden");
    expect(derive_app_state(WindowLifecycle{true, true, false, true, false}, false) ==
               AppState::Hidden,
           "an occluded window is Hidden");
    expect(derive_app_state(WindowLifecycle{false, true, false, false, false}, false) ==
               AppState::Hidden,
           "a hidden window is Hidden");
    expect(derive_app_state(WindowLifecycle{true, true, false, false, true}, false) ==
               AppState::Hidden,
           "a closed window is Hidden");
    // Suspension outranks every window rule: a frozen process may be killed, so the state says so.
    expect(derive_app_state(WindowLifecycle{true, true, false, false, false}, true) ==
               AppState::Suspended,
           "a suspended app is Suspended even while visible and focused");
    expect(derive_app_state(WindowLifecycle{true, true, true, false, false}, true) ==
               AppState::Suspended,
           "suspension outranks minimization");

    // The three states the derivation must never produce: Launching is the pre-start state and the
    // two termination states are the gate's output, so a window event can never fake them.
    const WindowLifecycle all[] = {
        WindowLifecycle{true, true, false, false, false},
        WindowLifecycle{true, false, false, false, false},
        WindowLifecycle{true, true, true, false, false},
        WindowLifecycle{true, true, false, true, false},
        WindowLifecycle{false, false, false, false, false},
        WindowLifecycle{true, true, false, false, true},
    };
    for (const WindowLifecycle& window : all) {
        for (const bool suspended : {false, true}) {
            const AppState derived = derive_app_state(window, suspended);
            expect(derived != AppState::Launching && derived != AppState::Terminating &&
                       derived != AppState::Terminated,
                   "the derivation never produces Launching or a termination state");
        }
    }
}

void test_fold() {
    // Minimize and occlude are independent reasons for Hidden, and a restore clears both. A restore
    // does not move focus: the ADR derives the state from "visible and focused", so a window
    // restored while another owns the keyboard is Inactive, not Active.
    expect(derive_app_state(after({LifecycleSignal::Minimize}), false) == AppState::Hidden,
           "Minimize -> Hidden");
    expect(derive_app_state(after({LifecycleSignal::Occlude}), false) == AppState::Hidden,
           "Occlude -> Hidden");
    expect(derive_app_state(after({LifecycleSignal::Occlude, LifecycleSignal::Restore}), false) ==
               AppState::Active,
           "Restore clears occlusion and keeps focus");
    expect(derive_app_state(
               after({LifecycleSignal::Blur, LifecycleSignal::Occlude, LifecycleSignal::Restore}),
               false) == AppState::Inactive,
           "Restore keeps the lost focus: Inactive, not Active");
    expect(derive_app_state(after({LifecycleSignal::Minimize, LifecycleSignal::Restore}), false) ==
               AppState::Active,
           "Restore clears minimization");

    // A repeated signal is idempotent, which is what lets a platform report a state twice without
    // the services seeing two transitions.
    const WindowLifecycle once = after({LifecycleSignal::Blur});
    const WindowLifecycle twice = after({LifecycleSignal::Blur, LifecycleSignal::Blur});
    expect(once == twice, "a repeated Blur folds to the same aggregate");

    // Focus after a close reopens the window: the aggregate carries `closed`, and a focus is the
    // platform saying the window is on screen again.
    const WindowLifecycle reopened = after({LifecycleSignal::Close, LifecycleSignal::Focus});
    expect(!reopened.closed, "Focus clears the closed bit");
}

void test_names_round_trip() {
    constexpr LifecycleSignal kSignals[] = {
        LifecycleSignal::Focus,           LifecycleSignal::Blur,    LifecycleSignal::Minimize,
        LifecycleSignal::Occlude,         LifecycleSignal::Restore, LifecycleSignal::Close,
        LifecycleSignal::ShutdownRequest, LifecycleSignal::Suspend, LifecycleSignal::Resume,
    };
    for (const LifecycleSignal signal : kSignals) {
        LifecycleSignal parsed{};
        expect(lifecycle_signal_from_name(lifecycle_signal_name(signal), parsed) &&
                   parsed == signal,
               "a lifecycle signal name round-trips");
        expect(!lifecycle_signal_name(signal).empty() && lifecycle_signal_name(signal) != "unknown",
               "every lifecycle signal has a name");
    }
    LifecycleSignal unparsed{};
    expect(!lifecycle_signal_from_name("nonsense", unparsed), "an unknown signal name is rejected");

    constexpr AppState kStates[] = {
        AppState::Launching, AppState::Active,      AppState::Inactive,   AppState::Hidden,
        AppState::Suspended, AppState::Terminating, AppState::Terminated,
    };
    for (const AppState state : kStates) {
        AppState parsed{};
        expect(app_state_from_name(app_state_name(state), parsed) && parsed == state,
               "an app state name round-trips");
        expect(app_state_name(state) != "unknown", "every app state has a name");
    }
    AppState ignored{};
    expect(!app_state_from_name("nonsense", ignored), "an unknown state name is rejected");
}

void test_window_event_mapping() {
    // The layer-0 bridge: exactly the six lifecycle transitions map, everything else is dropped.
    // This is the seam ADR-050 section 1 rule 1 allows, so the assertion is that it is exactly six.
    constexpr tessera::hal::WindowEventType kLifecycle[] = {
        tessera::hal::WindowEventType::Focus,     tessera::hal::WindowEventType::Blur,
        tessera::hal::WindowEventType::Minimized, tessera::hal::WindowEventType::Occluded,
        tessera::hal::WindowEventType::Restored,  tessera::hal::WindowEventType::Close,
    };
    std::size_t mapped = 0;
    for (std::size_t index = 0; index < tessera::hal::kWindowEventTypeCount; ++index) {
        const auto type = static_cast<tessera::hal::WindowEventType>(index);
        LifecycleSignal signal{};
        if (!lifecycle_signal_for(type, signal)) {
            continue;
        }
        ++mapped;
        bool expected = false;
        for (const tessera::hal::WindowEventType lifecycle : kLifecycle) {
            expected = expected || lifecycle == type;
        }
        expect(expected, "only a lifecycle window event maps onto a lifecycle signal");
    }
    expect(mapped == 6, "six window event types map onto the lifecycle");

    LifecycleSignal signal{};
    expect(lifecycle_signal_for(tessera::hal::WindowEventType::Minimized, signal) &&
               signal == LifecycleSignal::Minimize,
           "Minimized maps to Minimize");
    expect(!lifecycle_signal_for(tessera::hal::WindowEventType::Resize, signal),
           "a resize is not a lifecycle transition");
    expect(!lifecycle_signal_for(tessera::hal::WindowEventType::ThemeChanged, signal),
           "a theme change is not a lifecycle transition");
}

void test_script_decoder() {
    using tessera::appfw::kMaxLifecycleScriptSteps;

    const std::string_view good = R"({
      "scriptVersion": 1,
      "script": "unit",
      "description": "three steps",
      "steps": [
        { "event": "focus", "expect": "active" },
        { "event": "blur", "expect": "inactive" },
        { "event": "suspend", "expect": "suspended" }
      ]
    })";
    auto script = parse_lifecycle_script(good);
    expect(script.has_value(), "a well-formed lifecycle script parses");
    if (script.has_value()) {
        expect(script->name == "unit", "the script name is read");
        expect(script->steps.size() == 3, "every step is read");
        expect(script->steps[1].event.signal == LifecycleSignal::Blur,
               "the step's event is decoded");
        expect(script->steps[1].expected == AppState::Inactive,
               "the step's expectation is decoded");
    }

    // All-or-nothing: every one of these rejects the whole document, which is what makes a typo
    // loud instead of silently turning a check into a no-op.
    const auto rejects = [](std::string_view text, LifecycleScriptErrorCode code,
                            std::string_view what) {
        const auto parsed = parse_lifecycle_script(text);
        expect(!parsed.has_value(), what);
        if (!parsed.has_value()) {
            expect(parsed.error().code == code, std::string(what) + " reports the documented code");
        }
    };
    rejects(R"({"scriptVersion": 2, "steps": [{"event":"focus","expect":"active"}]})",
            LifecycleScriptErrorCode::UnsupportedVersion, "a future version is refused");
    rejects(R"({"steps": [{"event":"focus","expect":"active"}]})",
            LifecycleScriptErrorCode::MalformedScript, "a missing version is refused");
    rejects(R"({"scriptVersion": 1, "steps": []})", LifecycleScriptErrorCode::MalformedScript,
            "an empty step list is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"explode","expect":"active"}]})",
            LifecycleScriptErrorCode::UnknownEvent, "an unknown event is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"focus","expect":"sleeping"}]})",
            LifecycleScriptErrorCode::UnknownState, "an unknown state is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"focus"}]})",
            LifecycleScriptErrorCode::InvalidStep, "a step without an expectation is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"focus","expect":"active","extra":1}]})",
            LifecycleScriptErrorCode::InvalidStep, "an unknown step member is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"focus","expect":"active"}], "x": 1})",
            LifecycleScriptErrorCode::MalformedScript, "an unknown document member is refused");
    rejects(R"({"scriptVersion": 1, "steps": [{"event":"focus","expect":"active"}]} trailing)",
            LifecycleScriptErrorCode::MalformedScript, "trailing content is refused");

    std::string too_many = R"({"scriptVersion": 1, "steps": [)";
    for (std::size_t index = 0; index <= kMaxLifecycleScriptSteps; ++index) {
        if (index != 0) {
            too_many += ',';
        }
        too_many += R"({"event":"focus","expect":"active"})";
    }
    too_many += "]}";
    rejects(too_many, LifecycleScriptErrorCode::ScriptTooLarge, "the step cap is enforced");
}

} // namespace

int run_lifecycle_tests() {
    const int before = g_failures;
    test_state_table();
    test_fold();
    test_names_round_trip();
    test_window_event_mapping();
    test_script_decoder();
    return g_failures - before;
}
