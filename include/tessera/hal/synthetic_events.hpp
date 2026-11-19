// SPDX-License-Identifier: MIT
//
// The synthetic event injector (docs/specs/os-events-v1.md sections 3 and 12; ADR-023 rule 8).
//
// A deterministic test scripts the OS event stream instead of driving a real desktop: the injector
// records normalized events with a monotonic sequence and a simulated timestamp, coalesces them by
// the same rules the platform queue uses, and hands them out at a tick boundary. Nothing here
// touches an OS API, so a golden run never calls a platform function (rule 3's determinism clause).
//
// It is a *producer* the tests own, not a HAL seam: the fake window (`tests/unit/harness`) is the
// `hal::Window` double the runtime loop already consumes, and it feeds this injector's output to
// `poll_events`. Keeping the injector platform-neutral and out of `tessera_hal_window` means a test
// can script a desktop sequence (resize storm, DPI change, hotplug) on any OS, including a Linux CI
// runner with no X server.

#pragma once

#include "tessera/hal/display.hpp"
#include "tessera/hal/event_reason.hpp"
#include "tessera/hal/os_event.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::hal {

// Bounded script capacity. A script is a test fixture; a longer one is a bug in the test, and the
// injector reports the overrun instead of allocating without bound.
inline constexpr std::size_t kMaxSyntheticEvents = 64;

// Why `push` refused an event.
enum class InjectStatus : std::uint8_t {
    Queued,
    // The script is full: the event was not recorded (the count is reported once by `overflows`).
    Full,
    // The type is not a known `OsEventType`: an unknown event is ignored with a diagnostic, never a
    // crash (ADR-023 rule 5).
    UnknownType,
};

// Bounded capacity of the input ring that carries the system-button input events.
inline constexpr std::size_t kMaxSyntheticInputEvents = 16;

// The hardware or system buttons a device profile offers (studio-device-profiles-v1.md section 2),
// and the normalized events each one becomes (section 4). The mapping is the injector's `press`;
// there is no new `OsEventType` for any of them.
enum class SystemButton : std::uint8_t {
    // `InputEventType::BackRequested`.
    Back,
    // `WindowHidden` then `Suspend`: the app is backgrounded and keeps its state.
    Home,
    // `WindowDeactivated`: focus is lost, the app keeps running.
    Recents,
    // `InputEventType::VolumeKey` (`Up` / `Down`); no default action.
    VolumeUp,
    VolumeDown,
    // `ScreenOff` while the screen is on, `ScreenOn` on the next press.
    Power,
};

// One step of a display-capability script. A display script changes state and emits the event the
// platform would have emitted; the injector applies the state so a later step sees it.
struct DisplayChange {
    OsEventType type = OsEventType::RefreshRateChanged;
    EventReason reason = EventReason::Unspecified;
    DisplayId display = kNoDisplayId;
    // The values the change sets. A field is applied only for the event types that own it, so one
    // script step can carry the resize and the DPI change a single OS message means.
    float refresh_hz = 0.0f;
    float dpi_scale = 0.0f;
    WindowRect work_area;
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
};

// The deterministic OS event injector.
//
// Every recorded event carries the next sequence number and the injector's simulated clock, and
// events that coalesce (resize, DPI, refresh, work area) merge into the newest recorded one exactly
// like the platform queue: a resize storm produces one event, not a hundred. Lifecycle events are
// never merged, so a script cannot lose a close or a suspend.
class SyntheticEventInjector {
public:
    SyntheticEventInjector() = default;

    // Records one normalized event. The metrics are the ones in force *after* the change, so a
    // consumer that only reads the event sees a consistent snapshot. Returns `Full` when the script
    // is at `kMaxSyntheticEvents`, `UnknownType` for an out-of-range type, and `Queued` otherwise.
    InjectStatus push(OsEventType type, EventReason reason = EventReason::Unspecified);
    // Overload that stamps the event with an explicit metrics snapshot (a scripted display change
    // the injector's own state cannot express, e.g. a synthetic cutout).
    InjectStatus push(OsEventType type, const WindowMetrics& metrics,
                      EventReason reason = EventReason::Unspecified);

    // Applies one display-capability script step and records the event it implies. The step changes
    // the injector's metrics (the display capabilities the next event carries) and emits exactly
    // one event, so a script reads like a list of OS changes.
    InjectStatus apply(const DisplayChange& change);

    // Records one system-button press as the normalized events it maps to (see `SystemButton`).
    // Home and Power change the injector's own state (`screen_on`), and Home, which emits two
    // events, is refused as a whole (`Full`) when both do not fit, never half-delivered. Timestamps
    // come from the simulated clock, so a replay is byte-identical.
    InjectStatus press(SystemButton button);

    // Records one normalized input event in the input ring, stamped with the simulated clock. Input
    // never coalesces here (Back and Volume are discrete), and a consumer delivers it after every
    // OS event of the same tick, which is os-events-v1.md section 3's fixed order (lifecycle, then
    // display, then fold, then input).
    InjectStatus push_input(ui::InputEvent event);
    // Removes and returns the oldest input event; false when the ring is empty.
    [[nodiscard]] bool pop_input(ui::InputEvent& out) noexcept;
    // Moves pending input events, oldest first, into `queue` (the pump's queue). Returns how many
    // were pushed; an event the queue refuses stays pending.
    std::size_t feed_input(ui::InputQueue& queue);
    [[nodiscard]] std::size_t pending_input() const noexcept {
        return input_count_;
    }
    // False between a Power press that turned the screen off and the one that turns it back on.
    [[nodiscard]] bool screen_on() const noexcept {
        return screen_on_;
    }

    // Removes and returns the next queued event, or an empty optional when the script is done.
    [[nodiscard]] bool pop(OsEvent& out) noexcept;
    // Copies up to `out.size()` queued events (oldest first) and removes them, like
    // `hal::Window::poll_events`. Returns the count written.
    std::size_t drain(std::span<OsEvent> out) noexcept;

    [[nodiscard]] std::size_t pending() const noexcept {
        return count_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return count_ == 0;
    }
    // How many `push`/`apply` calls were refused because the script was full. A test asserts it is
    // 0; an overflow is a script bug, never silently dropped input.
    [[nodiscard]] std::size_t overflows() const noexcept {
        return overflows_;
    }
    // How many pushed events were merged into an earlier one by the coalescing rule. A resize-storm
    // test asserts the script saw many and the queue holds one.
    [[nodiscard]] std::size_t coalesced() const noexcept {
        return coalesced_;
    }

    // The metrics in force; a script step updates them and every following event carries them.
    [[nodiscard]] const WindowMetrics& metrics() const noexcept {
        return metrics_;
    }
    void set_metrics(const WindowMetrics& metrics) noexcept {
        metrics_ = metrics;
    }
    // The simulated monotonic clock, advanced by `advance_ms`/`advance_ns`. No wall clock is read,
    // so a replay produces byte-identical timestamps.
    [[nodiscard]] std::int64_t now_ns() const noexcept {
        return now_ns_;
    }
    void advance_ns(std::int64_t delta) noexcept;
    void advance_ms(std::int64_t delta) noexcept;

    // The next sequence value the injector will stamp (1-based; 0 is never issued).
    [[nodiscard]] std::uint64_t last_sequence() const noexcept {
        return sequence_.newest();
    }

    void clear() noexcept;

private:
    InjectStatus record(OsEvent event);

    std::array<OsEvent, kMaxSyntheticEvents> events_{};
    std::array<ui::InputEvent, kMaxSyntheticInputEvents> inputs_{};
    std::size_t input_head_ = 0;
    std::size_t input_count_ = 0;
    bool screen_on_ = true;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t overflows_ = 0;
    std::size_t coalesced_ = 0;
    OsEventSequence sequence_;
    std::int64_t now_ns_ = 0;
    WindowMetrics metrics_;
};

// --- Display-capability scripts ----------------------------------------------------------------
//
// The scripts the acceptance criteria name explicitly. They are ordinary functions returning a
// value, so a test can also build its own sequence; these are the named fixtures and keep the
// Windows/Linux parity tests reading the same script.

// A resize/DPI storm: many rapid resize and DPI notifications. The injector must coalesce them into
// the newest value per type, and the final metrics must be the last step's.
[[nodiscard]] SyntheticEventInjector make_resize_dpi_storm_script();

// A display hotplug: the window's monitor is removed and a new one (another scale, another refresh)
// takes over, then the topology settles.
[[nodiscard]] SyntheticEventInjector make_display_hotplug_script();

// A scale change with no size change: a compositor or per-monitor scale change.
[[nodiscard]] SyntheticEventInjector make_scale_change_script();

// A refresh-rate change (a mode switch or a move to a 120 Hz monitor).
[[nodiscard]] SyntheticEventInjector make_refresh_rate_change_script();

// --- System-button scripts ---------------------------------------------------------------------
//
// The named fixtures of studio-device-profiles-v1.md section 4 (issue #445). Each returns a loaded
// injector; the OS events are drained with `pop`/`drain` and the input events with `pop_input` or
// `feed_input`, OS events first.

// Back three times: the app state decides what each does (dismiss an overlay, pop a route, then
// nothing left: unhandled). The events themselves are identical.
[[nodiscard]] SyntheticEventInjector make_back_pop_back_exit_script();

// Home, then a return to the app: `WindowHidden`, `Suspend`, then `Resume`, `WindowShown`. A hot
// resume: the app keeps its state across it.
[[nodiscard]] SyntheticEventInjector make_home_resume_script();

// Power off and on: `ScreenOff`, then `ScreenOn` after a simulated second.
[[nodiscard]] SyntheticEventInjector make_power_cycle_script();

// Home with no return: `WindowHidden`, `Suspend`, and then the process dies. Death delivers no
// event (spec section 4: a session restart with a saved-state record), so the script ends at the
// `Suspend` that the saved-state capture keys on.
[[nodiscard]] SyntheticEventInjector make_process_death_script();

} // namespace tessera::hal
