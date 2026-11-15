// SPDX-License-Identifier: MIT
//
// The normalized OS event taxonomy (docs/specs/os-events-v1.md sections 2 and 3).
//
// This translation unit owns the three rules every consumer depends on: which category an event
// belongs to (the fixed delivery order), whether consecutive ones coalesce, and whether a visible
// change makes the event a wake source. The window-path rules are in `window_events.cpp`, because
// the window path already ships and has its own projection.
//
// The taxonomy is a table indexed by enumerator value: a row that drifts from the enum order, or an
// enumerator added without one, fails the build instead of becoming a silent "unknown".

#include "tessera/hal/os_event.hpp"

#include <array>

namespace tessera::hal {
namespace {

struct OsEventInfo {
    OsEventCategory category;
    bool coalesces;
    bool requests_frame;
    std::string_view name;
};

// Indexed by `static_cast<std::size_t>(OsEventType)`: the table is *the* taxonomy.
constexpr std::array<OsEventInfo, kOsEventTypeCount> kOsEvents{{
    // Lifecycle (delivered first).
    {OsEventCategory::Lifecycle, false, true, "WindowCreated"},
    {OsEventCategory::Lifecycle, false, true, "WindowShown"},
    {OsEventCategory::Lifecycle, false, true, "WindowHidden"},
    {OsEventCategory::Lifecycle, false, true, "WindowClosed"},
    {OsEventCategory::Lifecycle, false, true, "WindowActivated"},
    {OsEventCategory::Lifecycle, false, true, "WindowDeactivated"},
    {OsEventCategory::Lifecycle, false, true, "WindowFocused"},
    {OsEventCategory::Lifecycle, false, true, "WindowBlurred"},
    {OsEventCategory::Lifecycle, true, true, "WindowResized"},
    {OsEventCategory::Lifecycle, true, true, "WindowMoved"},
    {OsEventCategory::Lifecycle, false, true, "WindowStateChanged"},
    // Display / DPI / orientation (delivered second).
    {OsEventCategory::Display, true, true, "DpiChanged"},
    {OsEventCategory::Display, true, true, "ScaleChanged"},
    {OsEventCategory::Display, true, true, "OrientationChanged"},
    {OsEventCategory::Display, false, true, "DisplayAdded"},
    {OsEventCategory::Display, false, true, "DisplayRemoved"},
    {OsEventCategory::Display, true, true, "DisplayChanged"},
    {OsEventCategory::Display, true, true, "DisplayModeChanged"},
    {OsEventCategory::Display, true, true, "RefreshRateChanged"},
    {OsEventCategory::Display, true, true, "VrrChanged"},
    {OsEventCategory::Display, false, true, "HdrChanged"},
    {OsEventCategory::Display, false, true, "ColorGamutChanged"},
    {OsEventCategory::Display, true, true, "SafeAreaChanged"},
    {OsEventCategory::Display, true, true, "WorkAreaChanged"},
    // Fold / posture (delivered third; post-V1 backends, the type is normative now).
    {OsEventCategory::Fold, false, true, "FoldChanged"},
    {OsEventCategory::Fold, false, true, "PostureChanged"},
    {OsEventCategory::Fold, false, true, "MultiWindowChanged"},
    {OsEventCategory::Fold, false, true, "PictureInPictureChanged"},
    // Input devices, IME and user settings.
    {OsEventCategory::Settings, false, false, "InputDeviceChanged"},
    {OsEventCategory::Settings, true, true, "ImeChanged"},
    {OsEventCategory::Settings, false, true, "ThemeChanged"},
    {OsEventCategory::Settings, false, true, "AccentColorChanged"},
    {OsEventCategory::Settings, false, true, "HighContrastChanged"},
    {OsEventCategory::Settings, false, true, "ReducedMotionChanged"},
    {OsEventCategory::Settings, false, true, "TextScaleChanged"},
    {OsEventCategory::Settings, false, false, "LocaleChanged"},
    // Power, session and system (delivered last).
    {OsEventCategory::Power, false, false, "PowerSourceChanged"},
    {OsEventCategory::Power, false, true, "BatterySaverChanged"},
    {OsEventCategory::Power, false, true, "ThermalStateChanged"},
    {OsEventCategory::Power, false, false, "Suspend"},
    {OsEventCategory::Power, false, true, "Resume"},
    {OsEventCategory::Power, false, false, "ScreenOff"},
    {OsEventCategory::Power, false, true, "ScreenOn"},
    {OsEventCategory::System, false, false, "SessionLocked"},
    {OsEventCategory::System, false, true, "SessionUnlocked"},
    {OsEventCategory::System, false, false, "MemoryPressure"},
    {OsEventCategory::System, false, false, "ScreenCaptureChanged"},
    {OsEventCategory::System, false, true, "ShutdownRequested"},
}};

constexpr bool names_are_unique() {
    for (std::size_t i = 0; i < kOsEvents.size(); ++i) {
        if (kOsEvents[i].name.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < kOsEvents.size(); ++j) {
            if (kOsEvents[i].name == kOsEvents[j].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(kOsEvents.size() == kOsEventTypeCount,
              "every OsEventType enumerator needs exactly one row in kOsEvents");
static_assert(names_are_unique(), "the taxonomy names must be unique");

constexpr const OsEventInfo& info_for(OsEventType type) noexcept {
    const auto index = static_cast<std::size_t>(type);
    return kOsEvents[index < kOsEvents.size() ? index : kOsEvents.size() - 1];
}

} // namespace

bool os_event_is_known(OsEventType type) noexcept {
    return static_cast<std::size_t>(type) < kOsEventTypeCount;
}

OsEventCategory os_event_category(OsEventType type) noexcept {
    // An unknown value sorts last so it can never jump ahead of a lifecycle transition.
    return os_event_is_known(type) ? info_for(type).category : OsEventCategory::System;
}

int os_event_order_rank(OsEventType type) noexcept {
    switch (os_event_category(type)) {
    case OsEventCategory::Lifecycle:
        return 0;
    case OsEventCategory::Display:
        return 1;
    case OsEventCategory::Fold:
        return 2;
    case OsEventCategory::Settings:
        return 3;
    case OsEventCategory::Power:
        return 4;
    case OsEventCategory::System:
        return 5;
    }
    return 6;
}

std::string_view os_event_type_name(OsEventType type) noexcept {
    return os_event_is_known(type) ? info_for(type).name : std::string_view("unknown");
}

bool os_event_coalesces(OsEventType type) noexcept {
    return os_event_is_known(type) && info_for(type).coalesces;
}

bool os_event_requests_frame(OsEventType type) noexcept {
    // An unknown value must not wake a settled loop: it changes nothing this build can apply.
    return os_event_is_known(type) && info_for(type).requests_frame;
}

bool os_event_sequence_is_stale(std::uint64_t sequence, std::uint64_t newest) noexcept {
    return sequence != 0 && sequence <= newest;
}

OsEvent to_os_event(const WindowEvent& event, std::uint64_t sequence,
                    std::int64_t timestamp_ns) noexcept {
    OsEvent out;
    out.type = os_event_type_for(event.type);
    out.sequence = sequence;
    out.timestamp_ns = timestamp_ns;
    out.window = kPrimaryWindowId;
    out.display = event.metrics.display;
    // The event's own reason is authoritative; a platform that recorded none inherits the one the
    // window path already carries (it has the same field).
    out.reason = event.reason;
    out.metrics = event.metrics;
    out.fold = event.metrics.fold;
    out.handled = false;
    return out;
}

} // namespace tessera::hal
