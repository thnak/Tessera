// SPDX-License-Identifier: MIT
//
// The app lifecycle vocabulary (ADR-050 section 2). Three things live here, and nothing else:
//
//   * `AppState` — the seven states of the ADR's state diagram;
//   * `LifecycleSignal`/`LifecycleEvent` — the normalized input the state machine consumes, plus
//   the
//     mapping from the layer-0 window event a driver forwards, so the machine, its tests and its
//     lifecycle script never branch on a platform type;
//   * `derive_app_state` — the ADR's "desktop source" column as one pure function, so "event
//   scripts
//     produce the state table of section 2" is checkable row by row and not only end to end.
//
// The derivation is deliberately not an event-to-event transition table: section 2 derives each
// state from aggregate window state ("a window focused", "all windows blurred", "no visible
// window"), so a repeated `Blur`, or a `Restore` that arrives while the window is still occluded,
// has to land in the same state as the first one. With one window (`WindowHost` is one window per
// host) the aggregate is `WindowLifecycle`.

#pragma once

#include "tessera/hal/os_event.hpp"
#include "tessera/hal/window.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::appfw {

// ADR-050 section 2:
//
//   Launching -> Active <-> Inactive <-> Hidden <-> Suspended
//   any state except Terminated -> Terminating -> Terminated
//
// `Launching` covers P0-P3 of startup-api-v1.md and ends when the start sequence has run; AF3 keeps
// it while `ServiceRunner` is still `Starting`. `Terminating`/`Terminated` are reached only through
// the termination gate, because the ADR makes that a one-way edge out of every other state.
enum class AppState : std::uint8_t {
    Launching,
    Active,
    Inactive,
    Hidden,
    Suspended,
    Terminating,
    Terminated,
};

// The stable lowercase spelling diagnostics and lifecycle scripts use ("active", "terminating",
// ...).
[[nodiscard]] constexpr std::string_view app_state_name(AppState state) noexcept {
    switch (state) {
    case AppState::Launching:
        return "launching";
    case AppState::Active:
        return "active";
    case AppState::Inactive:
        return "inactive";
    case AppState::Hidden:
        return "hidden";
    case AppState::Suspended:
        return "suspended";
    case AppState::Terminating:
        return "terminating";
    case AppState::Terminated:
        return "terminated";
    }
    return "unknown";
}

// The inverse of `app_state_name`, for the lifecycle script and its report.
[[nodiscard]] constexpr bool app_state_from_name(std::string_view name, AppState& out) noexcept {
    constexpr AppState kAll[] = {
        AppState::Launching, AppState::Active,      AppState::Inactive,   AppState::Hidden,
        AppState::Suspended, AppState::Terminating, AppState::Terminated,
    };
    for (const AppState state : kAll) {
        if (app_state_name(state) == name) {
            out = state;
            return true;
        }
    }
    return false;
}

// The portable lifecycle input: one enumerator per normalized source the ADR's table names.
enum class LifecycleSignal : std::uint8_t {
    Focus,           // the window became the focused one
    Blur,            // the window lost focus while staying visible
    Minimize,        // the window is minimized, and therefore not visible
    Occlude,         // the window is fully occluded by another
    Restore,         // minimized/occluded -> visible again
    Close,           // the window asked to close
    ShutdownRequest, // the app (or the OS) asked the app to terminate
    Suspend,         // the OS is about to freeze the process
    Resume,          // the process is running again
};

[[nodiscard]] constexpr std::string_view lifecycle_signal_name(LifecycleSignal signal) noexcept {
    switch (signal) {
    case LifecycleSignal::Focus:
        return "focus";
    case LifecycleSignal::Blur:
        return "blur";
    case LifecycleSignal::Minimize:
        return "minimize";
    case LifecycleSignal::Occlude:
        return "occlude";
    case LifecycleSignal::Restore:
        return "restore";
    case LifecycleSignal::Close:
        return "close";
    case LifecycleSignal::ShutdownRequest:
        return "shutdown";
    case LifecycleSignal::Suspend:
        return "suspend";
    case LifecycleSignal::Resume:
        return "resume";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool lifecycle_signal_from_name(std::string_view name,
                                                        LifecycleSignal& out) noexcept {
    constexpr LifecycleSignal kAll[] = {
        LifecycleSignal::Focus,           LifecycleSignal::Blur,    LifecycleSignal::Minimize,
        LifecycleSignal::Occlude,         LifecycleSignal::Restore, LifecycleSignal::Close,
        LifecycleSignal::ShutdownRequest, LifecycleSignal::Suspend, LifecycleSignal::Resume,
    };
    for (const LifecycleSignal signal : kAll) {
        if (lifecycle_signal_name(signal) == name) {
            out = signal;
            return true;
        }
    }
    return false;
}

struct LifecycleEvent {
    LifecycleSignal signal = LifecycleSignal::Focus;

    friend constexpr bool operator==(const LifecycleEvent&, const LifecycleEvent&) = default;
};

// A service's answer to a termination request (ADR-050 section 2, "Termination request"): `Defer`
// is a service with unsaved work, which surfaces as the deferred outcome of
// `Application::request_termination()` so the app can ask the user instead of terminating.
enum class TerminateVote : std::uint8_t {
    Allow,
    Defer,
};

[[nodiscard]] constexpr std::string_view terminate_vote_name(TerminateVote vote) noexcept {
    return vote == TerminateVote::Allow ? "allow" : "defer";
}

// The single window of this application, as the state machine sees it. `closed` is only set when
// `quitOnLastWindowClosed` is false: with the configured default, closing the last window is a
// termination request rather than a `Hidden` state, and `Application` applies that rule before it
// folds the event.
struct WindowLifecycle {
    bool visible = true;
    bool focused = true;
    bool minimized = false;
    bool occluded = false;
    bool closed = false;

    friend constexpr bool operator==(const WindowLifecycle&, const WindowLifecycle&) = default;
};

// Folds one signal into the aggregate window state. `Close`/`ShutdownRequest` (the termination
// gate) and `Suspend`/`Resume` (the suspension flag) are handled by `Application`, so they leave
// the aggregate untouched here.
[[nodiscard]] constexpr WindowLifecycle fold_window_lifecycle(WindowLifecycle window,
                                                              LifecycleSignal signal) noexcept {
    switch (signal) {
    case LifecycleSignal::Focus:
        window.visible = true;
        window.focused = true;
        window.closed = false;
        return window;
    case LifecycleSignal::Blur:
        window.focused = false;
        return window;
    case LifecycleSignal::Minimize:
        window.minimized = true;
        window.occluded = false;
        return window;
    case LifecycleSignal::Occlude:
        window.occluded = true;
        return window;
    case LifecycleSignal::Restore:
        // Visible again, on both axes: a restore clears the minimize and the occlusion, but it does
        // not move focus. A window restored while another owns the keyboard is `Inactive`, which is
        // exactly the row the ADR's table derives from "visible and focused".
        window.visible = true;
        window.minimized = false;
        window.occluded = false;
        window.closed = false;
        return window;
    case LifecycleSignal::Close:
        window.closed = true;
        return window;
    case LifecycleSignal::ShutdownRequest:
    case LifecycleSignal::Suspend:
    case LifecycleSignal::Resume:
        return window;
    }
    return window;
}

// The ADR-050 section 2 table, one rule per row, in the ADR's precedence order:
//
//   Suspended   the process is frozen or may be killed
//   Hidden      no visible window (minimized, occluded, hidden, or closed)
//   Active      the window is visible and focused
//   Inactive    the window is visible and not focused
//
// `Launching`, `Terminating` and `Terminated` are deliberately not derived: `Launching` is the
// state before the start sequence has run, and the two termination states are the gate's output, so
// deriving them from window state would let a `Focus` undo a shutdown.
[[nodiscard]] constexpr AppState derive_app_state(const WindowLifecycle& window,
                                                  bool suspended) noexcept {
    if (suspended) {
        return AppState::Suspended;
    }
    if (window.closed || window.minimized || window.occluded || !window.visible) {
        return AppState::Hidden;
    }
    return window.focused ? AppState::Active : AppState::Inactive;
}

// Whether a transition is a "graceful stop" one, which dispatches in reverse start order (the rule
// ADR-050 section 4 item 1 pins for `stop()`): a dependent suspends before the dependency it uses,
// and resumes after it. `Terminating` belongs here for the same reason - it is the transition that
// tells every service a shutdown is coming, so it runs in the order the stop sequence will. Every
// other transition dispatches in start order, so a dependency observes a new state before the
// dependents that read it.
[[nodiscard]] constexpr bool dispatches_in_reverse(AppState state) noexcept {
    return state == AppState::Suspended || state == AppState::Terminating;
}

// The layer-0 bridge: the window event types ADR-050 section 2 derives the desktop state from,
// mapped onto the portable vocabulary. `false` means the event carries no lifecycle transition (a
// resize, a display change, a theme change) and must not reach the state machine at all.
//
// This is the whole platform surface of the lifecycle: `app::LifecycleSink` forwards one
// `hal::WindowEvent`, and nothing above this function names a platform type.
[[nodiscard]] constexpr bool lifecycle_signal_for(hal::WindowEventType type,
                                                  LifecycleSignal& out) noexcept {
    switch (type) {
    case hal::WindowEventType::Focus:
        out = LifecycleSignal::Focus;
        return true;
    case hal::WindowEventType::Blur:
        out = LifecycleSignal::Blur;
        return true;
    case hal::WindowEventType::Minimized:
        out = LifecycleSignal::Minimize;
        return true;
    case hal::WindowEventType::Occluded:
        out = LifecycleSignal::Occlude;
        return true;
    case hal::WindowEventType::Restored:
        out = LifecycleSignal::Restore;
        return true;
    case hal::WindowEventType::Close:
        out = LifecycleSignal::Close;
        return true;
    default:
        return false;
    }
}

// The same bridge from the normalized OS event, for the system-button mappings of
// studio-device-profiles-v1.md section 4 (issue #445): Home is `WindowHidden` then `Suspend`, and the
// way back is `Resume` then `WindowShown`; Recents is `WindowDeactivated`. `WindowHidden` is the
// no-visible-window row of ADR-050 section 2 (`Occlude`: it derives `Hidden`, exactly as a minimize
// does), and `WindowShown` is the `Restore` that clears it. Display, theme, power-source and screen
// events carry no lifecycle transition here (`ScreenOff`/`ScreenOn` pause presenting through the
// pacing path, not the app state), so `false` means "do not feed the state machine".
[[nodiscard]] constexpr bool lifecycle_signal_for(hal::OsEventType type,
                                                  LifecycleSignal& out) noexcept {
    switch (type) {
    case hal::OsEventType::WindowActivated:
        out = LifecycleSignal::Focus;
        return true;
    case hal::OsEventType::WindowDeactivated:
        out = LifecycleSignal::Blur;
        return true;
    case hal::OsEventType::WindowHidden:
        out = LifecycleSignal::Occlude;
        return true;
    case hal::OsEventType::WindowShown:
        out = LifecycleSignal::Restore;
        return true;
    case hal::OsEventType::Suspend:
        out = LifecycleSignal::Suspend;
        return true;
    case hal::OsEventType::Resume:
        out = LifecycleSignal::Resume;
        return true;
    case hal::OsEventType::ShutdownRequested:
        out = LifecycleSignal::ShutdownRequest;
        return true;
    default:
        return false;
    }
}

} // namespace tessera::appfw
