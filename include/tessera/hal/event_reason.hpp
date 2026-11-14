// SPDX-License-Identifier: MIT
//
// Why a normalized event happened (docs/specs/os-events-v1.md section 2, ADR-023 rule 5).
//
// The reason is a value the platform records next to the event; it never carries an OS message id
// or a parameter. Coalescing merges reasons instead of dropping them, so one `WindowResized` at a
// tick boundary can say "the user dragged and the monitor changed" (section 3, "union of reasons").
// This header is separate from `tessera/hal/display.hpp` because the window-path event type in
// `tessera/hal/window.hpp` needs it too, and that header must not depend on the display model.

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::hal {

// The cause of a normalized event. `Unspecified` is the default and the honest answer for a backend
// that only knows "something changed"; it never overrides a recorded reason when events coalesce.
enum class EventReason : std::uint8_t {
    Unspecified,
    // A direct user action: a drag, a keyboard window-menu command, a title-bar control.
    User,
    // The window manager, compositor or OS policy did it (snap, tiling, a scripted resize).
    System,
    // A monitor DPI/scale change (`WM_DPICHANGED`, a RandR output scale change).
    Dpi,
    // A display topology, mode or work-area change (hotplug, resolution, taskbar move).
    Display,
    // A display was added or removed. Coalescing keeps this reason ahead of `Display`.
    Hotplug,
    // The window entered the maximized state.
    Maximize,
    // The window left the maximized/full-screen state.
    Restore,
    // The window was minimized.
    Minimize,
    // The window entered or left full-screen.
    Fullscreen,
    // The app itself asked (set_fullscreen, a programmatic metrics push).
    Programmatic,
};

// The human-readable name ("user", "dpi", ...); used by the metrics dump, diagnostics and tests.
[[nodiscard]] std::string_view event_reason_name(EventReason reason) noexcept;

// The reason a coalesced event reports: the newest reason wins unless it is `Unspecified`, in which
// case the earlier recorded one is kept. This is the documented "union of reasons" for a single
// reason field: a later, more specific cause is never lost to a later generic notification.
[[nodiscard]] EventReason merge_event_reason(EventReason previous, EventReason newest) noexcept;

// True when `reason` is one of the enumerators above. An out-of-range value comes from a corrupt
// platform value or a bad script; callers ignore it with a diagnostic instead of crashing.
[[nodiscard]] bool event_reason_is_known(EventReason reason) noexcept;

} // namespace tessera::hal
