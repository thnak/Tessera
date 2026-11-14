// SPDX-License-Identifier: MIT
//
// The normalized OS event model (docs/specs/os-events-v1.md section 2; ADR-023 rules 1, 3, 4 and
// 7). Platform callbacks only record these values; the runtime loop consumes them at a tick
// boundary, in the fixed order of section 3, and nothing above the HAL ever sees an OS message.
//
// The taxonomy below is the normative one: one enumerator per normalized event, in the spec's
// order. The window path (`hal::WindowEvent`, delivered by `hal::Window::poll_events`) is the
// already-landed projection of this model onto the window domain; `os_event_type_for` maps it
// totally, and the two stay in sync through that mapping (there is one taxonomy, one mapping, and
// a test that walks it). Input, power, fold and accessibility events do not have a window-path
// producer yet; their enumerators exist so the taxonomy is complete and so a later backend has a
// target to fill rather than a type to invent.

#pragma once

#include "tessera/hal/display.hpp"
#include "tessera/hal/event_reason.hpp"
#include "tessera/hal/window.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tessera::hal {

enum class OsEventType : std::uint8_t {
    // Window lifecycle (delivered first).
    WindowCreated,
    WindowShown,
    WindowHidden,
    WindowClosed,
    WindowActivated,
    WindowDeactivated,
    WindowFocused,
    WindowBlurred,
    WindowResized,
    WindowMoved,
    WindowStateChanged,
    // Display / DPI / orientation (delivered second).
    DpiChanged,
    ScaleChanged,
    OrientationChanged,
    DisplayAdded,
    DisplayRemoved,
    DisplayChanged,
    DisplayModeChanged,
    RefreshRateChanged,
    VrrChanged,
    HdrChanged,
    ColorGamutChanged,
    SafeAreaChanged,
    WorkAreaChanged,
    // Fold / posture (delivered third; post-V1 backends, the type is normative now).
    FoldChanged,
    PostureChanged,
    MultiWindowChanged,
    PictureInPictureChanged,
    // Input devices and IME.
    InputDeviceChanged,
    ImeChanged,
    // Style / accessibility settings.
    ThemeChanged,
    AccentColorChanged,
    HighContrastChanged,
    ReducedMotionChanged,
    TextScaleChanged,
    LocaleChanged,
    // Power, session and system.
    PowerSourceChanged,
    BatterySaverChanged,
    ThermalStateChanged,
    Suspend,
    Resume,
    ScreenOff,
    ScreenOn,
    SessionLocked,
    SessionUnlocked,
    MemoryPressure,
    ScreenCaptureChanged,
    ShutdownRequested,
};

// The number of `OsEventType` enumerators. The implementation's table is indexed by the enumerator
// value and compile-time checked against this count, so an enumerator added without a row fails the
// build instead of becoming a silent "unknown".
inline constexpr std::size_t kOsEventTypeCount = 48;

// The delivery category of an event; section 3's fixed order is these values in ascending order.
// Input is not a category: pointer/keyboard events travel the input queue, not `OsEvent`.
enum class OsEventCategory : std::uint8_t {
    Lifecycle,
    Display,
    Fold,
    Settings,
    Power,
    System,
};

// The category `type` belongs to. An unknown value is `System`, which sorts last: a value this
// build does not understand can never jump ahead of a lifecycle transition.
[[nodiscard]] OsEventCategory os_event_category(OsEventType type) noexcept;

// The category's position in the fixed delivery order (Lifecycle 0 ... System 5).
[[nodiscard]] int os_event_order_rank(OsEventType type) noexcept;

// The spec spelling of `type` ("WindowResized", ...), or "unknown" for an out-of-range value.
[[nodiscard]] std::string_view os_event_type_name(OsEventType type) noexcept;

// False for an out-of-range value; the caller ignores it and reports one diagnostic (rule 5).
[[nodiscard]] bool os_event_is_known(OsEventType type) noexcept;

// True for the types section 3 says to coalesce: resize, move, DPI, scale, orientation, refresh and
// work area; keep the latest value, merge the reasons. Lifecycle, fold, settings and power
// transitions are never dropped, so they return false.
[[nodiscard]] bool os_event_coalesces(OsEventType type) noexcept;

// True when the event can change visible output and is therefore a wake source in render-on-demand
// mode (rule 4). Pure state-only events (a locale change, a memory-pressure hint) return false.
[[nodiscard]] bool os_event_requests_frame(OsEventType type) noexcept;

// One normalized event. It is a plain value: no heap, no OS handle, cheap to copy into the loop's
// bounded buffer. `sequence` is monotonically increasing per window/display; a value at or below
// the newest one already delivered is stale and must be discarded.
struct OsEvent {
    OsEventType type = OsEventType::WindowResized;
    std::uint64_t sequence = 0;
    // Monotonic nanoseconds; a synthetic script supplies its own, so a replay is deterministic.
    std::int64_t timestamp_ns = 0;
    WindowId window = kPrimaryWindowId;
    DisplayId display = kNoDisplayId;
    WindowMetrics metrics;
    // The event's fold snapshot; mirrors `metrics.fold` and is kept for the spec's field list so a
    // consumer can read the event without reaching into the metrics.
    FoldInfo fold;
    EventReason reason = EventReason::Unspecified;
    // Set by a consumer that acted on the event; a coalescing stage never clears a handled flag.
    bool handled = false;
};

// Per-window (and, for display events, per-display) sequence source. `next()` is the value the
// emitter stamps on an event; `newest()` is the highest one issued.
struct OsEventSequence {
    std::uint64_t value = 0;

    [[nodiscard]] std::uint64_t newest() const noexcept {
        return value;
    }
    std::uint64_t next() noexcept {
        return ++value;
    }
};

// True when `sequence` is not newer than `newest`: a stale/replayed event the loop discards
// (section 3, "stale sequences are discarded"). 0 is the "no sequence" value and is never stale by
// itself, so an emitter that does not stamp events keeps working.
[[nodiscard]] bool os_event_sequence_is_stale(std::uint64_t sequence,
                                              std::uint64_t newest) noexcept;

// Builds one normalized event from a window-path event. The mapping is total
// (`os_event_type_for` never fails for a valid `WindowEventType`), and an out-of-range wire value
// maps to `WindowResized` only so the function stays total: callers reject it with
// `os_event_is_known`/`window_event_type_is_known` before delivering.
[[nodiscard]] OsEventType os_event_type_for(WindowEventType type) noexcept;

// True when `type` is one of the window path's enumerators (a corrupt platform value is not).
[[nodiscard]] bool window_event_type_is_known(WindowEventType type) noexcept;

// The spec spelling of a window-path type, for diagnostics.
[[nodiscard]] std::string_view window_event_type_name(WindowEventType type) noexcept;

// Converts one window-path event into the normalized model. `sequence` is the value the caller
// already took from its `OsEventSequence`, and `timestamp_ns` its monotonic timestamp, so the
// conversion stays pure and a replay is deterministic.
[[nodiscard]] OsEvent to_os_event(const WindowEvent& event, std::uint64_t sequence,
                                  std::int64_t timestamp_ns) noexcept;

// Coalesces a tick's raw window-path events into the section 3 rules: events whose type
// `os_event_coalesces` collapse into the first occurrence (latest metrics, merged reason), every
// other event is kept, and the result is stably ordered by category rank. `out` may alias the
// start of `raw`. Returns the count written; a short `out` keeps the earliest events, which are the
// lifecycle ones.
std::size_t coalesce_window_events(std::span<const WindowEvent> raw,
                                   std::span<WindowEvent> out) noexcept;

} // namespace tessera::hal
