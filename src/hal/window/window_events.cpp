// SPDX-License-Identifier: MIT
//
// Window-path event rules (docs/specs/os-events-v1.md sections 2 and 3; ADR-023 rules 5 and 7).
//
// The window path (`hal::WindowEvent` from `hal::Window::poll_events`) is the projection of the
// normalized model that already ships. This file owns what a consumer of that path needs without
// pulling in the whole `OsEvent` taxonomy: the category (and therefore the fixed delivery order),
// which types coalesce, the spec spelling for diagnostics, and the mapping onto `OsEventType`.
//
// A table indexed by enumerator value makes the mapping total: an enumerator added to
// `WindowEventType` without a row here is a compile error, so the two taxonomies cannot drift.

#include "tessera/hal/os_event.hpp"
#include "tessera/hal/window.hpp"

#include <array>

namespace tessera::hal {
namespace {

struct WindowEventInfo {
    bool coalesces;
    OsEventType os_type;
    std::string_view name;
};

// Indexed by `static_cast<std::size_t>(WindowEventType)`, in declaration order. There is no
// separate category column: the delivery category is the mapped `OsEventType`'s, so the window path
// cannot drift from the normalized model (section 3 has one order, not two).
constexpr std::array<WindowEventInfo, kWindowEventTypeCount> kWindowEvents{{
    {true, OsEventType::WindowResized, "Resize"},
    {true, OsEventType::DpiChanged, "DpiChange"},
    {false, OsEventType::WindowFocused, "Focus"},
    {false, OsEventType::WindowBlurred, "Blur"},
    // Occlusion and minimize are reported through the metrics and
    // `WindowHidden`/`WindowStateChanged` (section 2: there is no separate Occluded type in the
    // OsEvent taxonomy).
    {false, OsEventType::WindowHidden, "Occluded"},
    {false, OsEventType::WindowStateChanged, "Minimized"},
    {false, OsEventType::WindowShown, "Restored"},
    {false, OsEventType::WindowClosed, "Close"},
    // The system settings broadcast the app mirrors today is the reduced-motion toggle (the window
    // path re-queries the OS setting; the event carries no value).
    {false, OsEventType::ReducedMotionChanged, "SystemSettingsChanged"},
    {true, OsEventType::SafeAreaChanged, "SafeAreaChanged"},
    {true, OsEventType::ImeChanged, "ImeChanged"},
    {false, OsEventType::FoldChanged, "FoldChanged"},
    {true, OsEventType::RefreshRateChanged, "RefreshRateChanged"},
    {true, OsEventType::WorkAreaChanged, "WorkAreaChanged"},
    {false, OsEventType::DisplayAdded, "DisplayAdded"},
    {false, OsEventType::DisplayRemoved, "DisplayRemoved"},
    {true, OsEventType::DisplayChanged, "DisplayChanged"},
    {true, OsEventType::ScaleChanged, "ScaleChanged"},
}};

constexpr bool window_names_are_unique() {
    for (std::size_t i = 0; i < kWindowEvents.size(); ++i) {
        if (kWindowEvents[i].name.empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < kWindowEvents.size(); ++j) {
            if (kWindowEvents[i].name == kWindowEvents[j].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(kWindowEvents.size() == kWindowEventTypeCount,
              "every WindowEventType enumerator needs exactly one row in kWindowEvents");
static_assert(window_names_are_unique(), "the window event names must be unique");

constexpr const WindowEventInfo& window_info_for(std::size_t index) noexcept {
    return kWindowEvents[index < kWindowEvents.size() ? index : kWindowEvents.size() - 1];
}

} // namespace

WindowEventCategory window_event_category(WindowEventType type) noexcept {
    // The window path reports the normalized model's category: one taxonomy, not two. The window's
    // own category enum is kept for its two extra buckets (geometry and settings) that the
    // `OsEventType` table folds into Display/Settings.
    switch (os_event_category(os_event_type_for(type))) {
    case OsEventCategory::Lifecycle:
        return WindowEventCategory::Lifecycle;
    case OsEventCategory::Display:
        return WindowEventCategory::Display;
    case OsEventCategory::Fold:
        return WindowEventCategory::Geometry;
    case OsEventCategory::Settings:
        return WindowEventCategory::Settings;
    case OsEventCategory::Power:
    case OsEventCategory::System:
        return WindowEventCategory::Settings;
    }
    return WindowEventCategory::Settings;
}

int window_event_order_rank(WindowEventType type) noexcept {
    // The delivery order is the normalized taxonomy's (section 3), not a second table: the window
    // path is a projection of that model, so a type ranks by the `OsEventType` it maps to. That
    // keeps `WindowResized` in the lifecycle tier with the other window transitions and puts a
    // later-added window event in the right tier the moment its row exists.
    return os_event_order_rank(os_event_type_for(type));
}

bool window_event_coalesces(WindowEventType type) noexcept {
    return window_info_for(static_cast<std::size_t>(type)).coalesces;
}

bool window_event_type_is_known(WindowEventType type) noexcept {
    return static_cast<std::size_t>(type) < kWindowEventTypeCount;
}

std::string_view window_event_type_name(WindowEventType type) noexcept {
    return window_event_type_is_known(type) ? window_info_for(static_cast<std::size_t>(type)).name
                                            : std::string_view("unknown");
}

OsEventType os_event_type_for(WindowEventType type) noexcept {
    // Total by construction. An out-of-range wire value reads the last row, so callers reject it
    // with `window_event_type_is_known` before delivering (rule 5: ignore with a diagnostic).
    return window_info_for(static_cast<std::size_t>(type)).os_type;
}

std::size_t coalesce_window_events(std::span<const WindowEvent> raw,
                                   std::span<WindowEvent> out) noexcept {
    if (raw.empty() || out.empty()) {
        return 0;
    }
    // The platform queue is already bounded and coalesces the runs it can (window_event_queue.hpp);
    // this pass is the tick-boundary rule on top of it. The scratch is a fixed stack array sized
    // like the queue, so a tick's coalescing never allocates.
    std::array<WindowEvent, 64> scratch{};
    std::size_t count = 0;
    for (const WindowEvent& event : raw) {
        if (count == scratch.size()) {
            break;
        }
        if (count > 0) {
            WindowEvent& newest = scratch[count - 1];
            if (newest.type == event.type && window_event_coalesces(event.type)) {
                // Keep the latest value, merge the reasons: a later generic notification must not
                // erase the specific cause that started the run.
                const EventReason reason = merge_event_reason(newest.reason, event.reason);
                newest = event;
                newest.reason = reason;
                continue;
            }
        }
        scratch[count++] = event;
    }
    // Stable ordering by category rank: the sequence within a category is preserved, which is what
    // makes a replayed script deterministic.
    std::size_t written = 0;
    for (int rank = 0; rank <= 4 && written < out.size(); ++rank) {
        for (std::size_t index = 0; index < count && written < out.size(); ++index) {
            if (window_event_order_rank(scratch[index].type) != rank) {
                continue;
            }
            out[written++] = scratch[index];
        }
    }
    return written;
}

} // namespace tessera::hal
