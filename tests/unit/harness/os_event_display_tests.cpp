// SPDX-License-Identifier: MIT

// Synthetic display scripts (docs/specs/os-events-v1.md sections 3 and 12; ADR-023 rules 5-8): the
// resize/DPI storm, the display hotplug, a scale change and a refresh-rate change, driven through
// the deterministic `SyntheticEventInjector` and delivered to the runtime seam the app consumes
// (`hal::Window::poll_events` on the fake window).
//
// The acceptance criterion is "synthetic display scripts pass on Windows and Linux". This test is
// OS-neutral by construction - it never opens a window, a display, a device or a platform API - so
// it runs identically on both gating lanes. What it proves: the documented fallbacks (60 Hz / 1.0)
// are recorded as assumptions, coalescing keeps the newest value and merges the reasons, the fixed
// delivery order holds, an unknown event is ignored with no crash, and a replay is byte-identical.

#include "fake_hal.hpp"
#include "tessera/hal/os_event.hpp"
#include "tessera/hal/synthetic_events.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace tessera;
using hal::OsEvent;
using hal::OsEventType;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "os_event_display: " << message << "\n";
        ++failures;
    }
    return condition;
}

// Drains the injector into `out` and returns the count. The buffer is bounded like a real poll.
std::size_t drain(hal::SyntheticEventInjector& injector, std::span<OsEvent> out) {
    return injector.drain(out);
}

void check_resize_dpi_storm() {
    hal::SyntheticEventInjector injector = hal::make_resize_dpi_storm_script();
    // Twelve resize/DPI notifications in the script: the queue coalesces each contiguous run, so it
    // holds far fewer events than the script pushed.
    expect(injector.coalesced() > 0, "the storm coalesced the resize run");
    expect(injector.pending() < 12, "the storm is not a one-event-per-message queue");
    expect(injector.overflows() == 0, "the storm fits the bounded script");

    std::array<OsEvent, 16> events{};
    const std::size_t count = drain(injector, events);
    expect(count >= 2, "the storm keeps the resize and the DPI change");

    // The DPI change survived (it is its own run, not after another resize).
    bool saw_resize = false;
    bool saw_dpi = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (events[index].type == OsEventType::WindowResized) {
            saw_resize = true;
        }
        if (events[index].type == OsEventType::DpiChanged) {
            saw_dpi = true;
        }
    }
    expect(saw_resize, "the resize event survives");
    expect(saw_dpi, "the DPI change survives");
    // The final metrics are the last script step: 2100+90 physical wide at 1.5 scale.
    expect(injector.metrics().dpi_scale == 1.5f, "the final scale is the last step's");
    expect(injector.metrics().physical_width > 2000, "the final size is the last step's");

    // The fixed delivery order is applied when the runtime coalesces a tick's raw events (section
    // 3), not by the recorder: the injector keeps script order, and `coalesce_window_events` is
    // what the loop runs. Convert to the window path, coalesce, and assert the result is ordered.
    std::array<hal::WindowEvent, 16> raw{};
    for (std::size_t index = 0; index < count; ++index) {
        raw[index].type = hal::WindowEventType::Resize;
        raw[index].sequence = events[index].sequence;
        raw[index].metrics = events[index].metrics;
        // The taxonomy type maps back onto the window path for the coalescer's category lookup.
        if (events[index].type == OsEventType::DpiChanged) {
            raw[index].type = hal::WindowEventType::DpiChange;
        } else if (events[index].type == OsEventType::WindowResized) {
            raw[index].type = hal::WindowEventType::Resize;
        }
    }
    std::array<hal::WindowEvent, 16> ordered{};
    const std::size_t ordered_count =
        hal::coalesce_window_events(std::span<const hal::WindowEvent>(raw.data(), count), ordered);
    int last_rank = -1;
    bool in_order = true;
    for (std::size_t index = 0; index < ordered_count; ++index) {
        const int rank = hal::window_event_order_rank(ordered[index].type);
        if (rank < last_rank) {
            in_order = false;
            break;
        }
        last_rank = rank;
    }
    expect(in_order, "the coalesced tick is in the fixed delivery order");
    expect(count >= 2 && ordered_count >= 1, "the storm yields at least one coalesced event");
}

void check_display_hotplug() {
    hal::SyntheticEventInjector injector = hal::make_display_hotplug_script();
    std::array<OsEvent, 16> events{};
    const std::size_t count = drain(injector, events);
    expect(count >= 3, "the hotplug script has its lifecycle/display events");

    bool saw_removed = false;
    bool saw_added_or_changed = false;
    bool saw_refresh = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (events[index].type == OsEventType::DisplayRemoved) {
            saw_removed = true;
            expect(events[index].display == 1, "the removed event names display 1");
            expect(events[index].reason == hal::EventReason::Hotplug,
                   "the removal carries the hotplug reason");
        }
        if (events[index].type == OsEventType::DisplayChanged ||
            events[index].type == OsEventType::DisplayAdded ||
            events[index].type == OsEventType::ScaleChanged) {
            saw_added_or_changed = true;
        }
        if (events[index].type == OsEventType::RefreshRateChanged) {
            saw_refresh = true;
            expect(events[index].metrics.refresh_hz == 120.0f,
                   "the new display's refresh rides the event");
            expect(!events[index].metrics.assumed.refresh_hz,
                   "a measured refresh is not marked assumed");
        }
    }
    expect(saw_removed, "the removed display is reported");
    expect(saw_added_or_changed, "the window's move to the new display is reported");
    expect(saw_refresh, "the refresh change is reported");
    // The window landed on the 2x display, and the final state says so.
    expect(injector.metrics().display == 2, "the final display is the new one");
    expect(injector.metrics().dpi_scale == 2.0f, "the final scale is the new display's");

    // The recorder keeps script order; the runtime's coalescing pass applies the fixed delivery
    // order. Assert the coalesced result, which is what the loop delivers to the frame boundary.
    std::array<hal::WindowEvent, 16> raw{};
    for (std::size_t index = 0; index < count; ++index) {
        raw[index].metrics = events[index].metrics;
        switch (events[index].type) {
        case OsEventType::DisplayRemoved:
            raw[index].type = hal::WindowEventType::DisplayRemoved;
            break;
        case OsEventType::DisplayChanged:
            raw[index].type = hal::WindowEventType::DisplayChanged;
            break;
        case OsEventType::ScaleChanged:
            raw[index].type = hal::WindowEventType::ScaleChanged;
            break;
        case OsEventType::RefreshRateChanged:
            raw[index].type = hal::WindowEventType::RefreshRateChanged;
            break;
        default:
            raw[index].type = hal::WindowEventType::DisplayChanged;
            break;
        }
    }
    std::array<hal::WindowEvent, 16> ordered_events{};
    const std::size_t ordered_count = hal::coalesce_window_events(
        std::span<const hal::WindowEvent>(raw.data(), count), ordered_events);
    int last_rank = -1;
    bool in_order = true;
    for (std::size_t index = 0; index < ordered_count; ++index) {
        const int rank = hal::window_event_order_rank(ordered_events[index].type);
        if (rank < last_rank) {
            in_order = false;
        }
        last_rank = rank;
    }
    expect(in_order, "the coalesced hotplug tick is in the fixed delivery order");
}

void check_scale_change() {
    hal::SyntheticEventInjector injector = hal::make_scale_change_script();
    std::array<OsEvent, 8> events{};
    const std::size_t count = drain(injector, events);
    // Two ScaleChanged steps coalesce into the newest: 1.0, not 2.0.
    expect(count == 1, "consecutive scale changes coalesce into one");
    expect(events[0].type == OsEventType::ScaleChanged, "the surviving event is the scale change");
    expect(events[0].metrics.dpi_scale == 1.0f, "the coalesced scale is the newest value");
    expect(injector.metrics().dpi_scale == 1.0f, "the final scale is the newest value");
    expect(injector.coalesced() == 1, "exactly one scale change was merged");
}

void check_refresh_rate_change() {
    hal::SyntheticEventInjector injector = hal::make_refresh_rate_change_script();
    std::array<OsEvent, 8> events{};
    const std::size_t count = drain(injector, events);
    expect(count == 1, "consecutive refresh changes coalesce into one");
    expect(events[0].type == OsEventType::RefreshRateChanged,
           "the surviving event is the refresh change");
    // The newest value is the 59.94 Hz mode; it is carried as reported, not rounded to 60.
    expect(std::abs(events[0].metrics.refresh_hz - 59.94f) < 0.01f,
           "the coalesced refresh is the newest reported value");
    expect(!events[0].metrics.assumed.refresh_hz, "a reported refresh is not marked assumed");
}

void check_missing_capabilities_are_recorded() {
    // ADR-023 rule 7: missing refresh/scale assumes 60 Hz / 1.0 and *records* the assumption. A
    // backend that cannot report either leaves the metrics at the fallbacks with the flags set.
    hal::SyntheticEventInjector injector;
    hal::WindowMetrics metrics;
    metrics.logical_width = 800;
    metrics.logical_height = 600;
    injector.set_metrics(metrics);
    const hal::WindowMetrics& base = injector.metrics();
    expect(base.refresh_hz == hal::kAssumedDisplayRefreshHz,
           "a window with no display source reports the 60 Hz fallback");
    expect(base.assumed.refresh_hz, "the 60 Hz fallback is recorded as an assumption");
    expect(base.dpi_scale == hal::kAssumedDisplayScale, "the scale fallback is 1.0");
    expect(base.assumed.work_area, "the missing work area is recorded as an assumption");
    expect(base.assumed.orientation, "the derived orientation is recorded as an assumption");
    expect(base.display == hal::kNoDisplayId, "a platform that cannot report a display says so");

    // An unusable report is not a measurement: applying a NaN refresh keeps the fallback and the
    // flag.
    hal::DisplayChange bad_refresh;
    bad_refresh.type = OsEventType::RefreshRateChanged;
    bad_refresh.reason = hal::EventReason::Display;
    bad_refresh.refresh_hz = std::nanf("");
    (void)injector.apply(bad_refresh);
    expect(injector.metrics().refresh_hz == hal::kAssumedDisplayRefreshHz &&
               injector.metrics().assumed.refresh_hz,
           "a NaN refresh keeps the recorded 60 Hz assumption");
}

void check_unknown_event_is_ignored() {
    // ADR-023 rule 5: an unknown/unsupported event is ignored with a diagnostic, never a crash.
    hal::SyntheticEventInjector injector;
    expect(injector.push(static_cast<OsEventType>(200)) == hal::InjectStatus::UnknownType,
           "an out-of-range event type is refused");
    expect(injector.pending() == 0, "the unknown event is not queued");
    expect(injector.overflows() == 0, "an unknown event is not an overflow");

    // The taxonomy's own rules refuse it too: the loop would drop it rather than act on it.
    expect(!hal::os_event_is_known(static_cast<OsEventType>(200)),
           "the taxonomy does not know the event");
    expect(!hal::os_event_requests_frame(static_cast<OsEventType>(200)),
           "an unknown event is not a wake source");

    // An unknown value from the window path is likewise ignored by the consumer.
    expect(!hal::window_event_type_is_known(static_cast<hal::WindowEventType>(200)),
           "an out-of-range window event type is not known");
}

void check_injector_replay_is_deterministic() {
    // Rule 3: a replay of the same script produces the same events with the same timestamps and
    // sequence numbers. That is what makes the display scripts golden-mode safe (no platform API).
    hal::SyntheticEventInjector a = hal::make_resize_dpi_storm_script();
    hal::SyntheticEventInjector b = hal::make_resize_dpi_storm_script();
    std::vector<OsEvent> first(16);
    std::vector<OsEvent> second(16);
    const std::size_t count_a = a.drain(first);
    const std::size_t count_b = b.drain(second);
    expect(count_a == count_b, "the same script yields the same event count");
    bool identical = count_a == count_b;
    for (std::size_t index = 0; identical && index < count_a; ++index) {
        identical = first[index].type == second[index].type &&
                    first[index].sequence == second[index].sequence &&
                    first[index].timestamp_ns == second[index].timestamp_ns &&
                    first[index].metrics.physical_width == second[index].metrics.physical_width &&
                    first[index].metrics.refresh_hz == second[index].metrics.refresh_hz;
    }
    expect(identical, "the replay is byte-identical (type, sequence, timestamp, metrics)");
}

void check_injector_through_fake_window() {
    // The delivery seam: the app reads `hal::Window::poll_events` at a tick boundary. The fake
    // window is the same seam a platform backend fills, so a script converted to window events is
    // what the runtime loop sees. This proves the scripted events survive that conversion with
    // their order and coalescing intact.
    FakeWindow window;
    hal::WindowMetrics metrics = window.metrics();
    metrics.display = 3;
    metrics.refresh_hz = 144.0f;
    metrics.assumed.refresh_hz = false;
    window.set_metrics(metrics);
    window.set_display(3, 144.0f);

    const hal::WindowMetrics current = window.metrics();
    expect(current.display == 3 && current.refresh_hz == 144.0f,
           "the fake reports the scripted display caps");
    expect(!current.assumed.refresh_hz, "a measured refresh is not assumed");

    // A refresh change and a work-area change arrive at the seam and coalesce like a platform
    // queue.
    window.set_refresh_rate(165.0f);
    window.set_refresh_rate(240.0f);
    expect(window.queued_events() == 1, "consecutive refresh changes coalesce at the seam");
    window.set_work_area(hal::WindowRect{0, 30, 1920, 1050});
    expect(window.queued_events() == 2, "the work-area change is a separate event");

    std::array<hal::WindowEvent, 8> events{};
    const std::size_t count = window.poll_events(events);
    expect(count == 2, "both display events poll out");
    expect(events[0].type == hal::WindowEventType::RefreshRateChanged,
           "the refresh change is delivered");
    expect(events[0].metrics.refresh_hz == 240.0f, "the coalesced refresh is the newest value");
    expect(events[1].type == hal::WindowEventType::WorkAreaChanged,
           "the work-area change is delivered");
    expect(events[1].metrics.work_area.y == 30.0f, "the work area carries the reported rectangle");

    // The fixed delivery order: the display events precede a settings event queued after them.
    window.push_event(hal::WindowEventType::SystemSettingsChanged);
    std::array<hal::WindowEvent, 8> ordered{};
    const std::size_t ordered_count = window.poll_events(ordered);
    expect(ordered_count == 1, "the settings event polls out");
    expect(hal::window_event_order_rank(ordered[0].type) == 3, "settings sort last");

    // Topology: a hotplug replaces the list, and the fake reports the new monitors.
    hal::DisplayInfo primary;
    primary.id = 3;
    primary.primary = true;
    primary.refresh_hz = 240.0f;
    primary.refresh_hz_assumed = false;
    hal::DisplayInfo secondary;
    secondary.id = 4;
    secondary.refresh_hz = 60.0f;
    const hal::DisplayInfo list[] = {primary, secondary};
    window.set_displays(list);
    std::array<hal::DisplayInfo, hal::kMaxDisplays> reported{};
    expect(window.displays(reported) == 2, "the fake reports the new topology");
    expect(reported[0].id == 3 && reported[1].id == 4, "the monitors keep their order");
    expect(reported[0].primary && !reported[1].primary, "the primary flag is kept");

    // A hotplug event names the display that changed.
    window.push_display_event(hal::WindowEventType::DisplayAdded, 4);
    std::array<hal::WindowEvent, 8> hotplug{};
    expect(window.poll_events(hotplug) == 1, "the hotplug event polls out");
    expect(hotplug[0].type == hal::WindowEventType::DisplayAdded, "the add is reported");
    expect(hotplug[0].metrics.display == 4, "the added display is named");
}

void check_bounded_script() {
    // A script larger than the bounded capacity reports the overflow instead of growing without
    // bound; a test that scripts too much sees it rather than silently losing events.
    hal::SyntheticEventInjector injector;
    for (std::size_t index = 0; index < hal::kMaxSyntheticEvents + 5; ++index) {
        // Lifecycle events never coalesce, so each one takes a slot.
        (void)injector.push(OsEventType::Suspend);
    }
    expect(injector.overflows() == 5, "the overrun is counted");
    expect(injector.pending() == hal::kMaxSyntheticEvents, "the queue is capped at its capacity");
}

} // namespace

int main() {
    check_resize_dpi_storm();
    check_display_hotplug();
    check_scale_change();
    check_refresh_rate_change();
    check_missing_capabilities_are_recorded();
    check_unknown_event_is_ignored();
    check_injector_replay_is_deterministic();
    check_injector_through_fake_window();
    check_bounded_script();

    if (failures != 0) {
        std::cerr << "os_event_display: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "os_event_display: ok\n";
    return 0;
}
