// SPDX-License-Identifier: MIT
//
// Event reason values (docs/specs/os-events-v1.md section 2, ADR-023 rule 5).
//
// The reason a platform recorded, its name for diagnostics, and the coalescing rule: a later
// specific cause is never lost to a later generic notification. Pure and platform-neutral, so the
// merge rule is testable without an OS.

#include "tessera/hal/event_reason.hpp"

namespace tessera::hal {

std::string_view event_reason_name(EventReason reason) noexcept {
    switch (reason) {
    case EventReason::Unspecified:
        return "unspecified";
    case EventReason::User:
        return "user";
    case EventReason::System:
        return "system";
    case EventReason::Dpi:
        return "dpi";
    case EventReason::Display:
        return "display";
    case EventReason::Hotplug:
        return "hotplug";
    case EventReason::Maximize:
        return "maximize";
    case EventReason::Restore:
        return "restore";
    case EventReason::Minimize:
        return "minimize";
    case EventReason::Fullscreen:
        return "fullscreen";
    case EventReason::Programmatic:
        return "programmatic";
    }
    return "unknown";
}

EventReason merge_event_reason(EventReason previous, EventReason newest) noexcept {
    // A generic notification that follows a specific cause describes the same change: a
    // WM_SETTINGCHANGE after a WM_DPICHANGED is the work area moving because the DPI did. Keeping
    // the specific reason is what makes the coalesced event say why it really happened, and it is
    // the union section 3 asks for on a single reason field.
    if (newest == EventReason::Unspecified) {
        return previous;
    }
    if (newest == EventReason::System && previous != EventReason::Unspecified &&
        previous != EventReason::System) {
        return previous;
    }
    return newest;
}

bool event_reason_is_known(EventReason reason) noexcept {
    return static_cast<std::uint8_t>(reason) <=
           static_cast<std::uint8_t>(EventReason::Programmatic);
}

} // namespace tessera::hal
