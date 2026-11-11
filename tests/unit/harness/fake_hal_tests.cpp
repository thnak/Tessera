// SPDX-License-Identifier: MIT

#include "fake_hal.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using tessera::hal::WindowEvent;
using tessera::hal::WindowEventType;
using tessera::hal::WindowMetrics;
using tessera::harness::testing::default_fake_metrics;
using tessera::harness::testing::FakeWindow;
using tessera::harness::testing::kMaxFakeWindowEvents;
using tessera::harness::testing::ManualClock;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "fake_hal: " << message << "\n";
        ++failures;
    }
    return condition;
}

} // namespace

int main() {
    // The manual clock only moves when the script says so, and saturates instead of wrapping.
    {
        ManualClock clock;
        expect(clock.now_us() == 0, "clock starts at zero");
        clock.advance_us(250);
        expect(clock.now_us() == 250, "advance_us moves the clock");
        clock.advance_ms(2);
        expect(clock.now_us() == 2250, "advance_ms moves the clock");
        ManualClock started(5000);
        expect(started.now_us() == 5000, "start offset honored");
        started.advance_us(UINT64_MAX);
        expect(started.now_us() == UINT64_MAX, "clock saturates");
        started.advance_us(1);
        expect(started.now_us() == UINT64_MAX, "saturated clock stays put");
    }

    // Default metrics are a deterministic normalized snapshot, not a real window.
    {
        FakeWindow window;
        const WindowMetrics metrics = window.metrics();
        expect(metrics.logical_width == 1280 && metrics.logical_height == 720,
               "default logical size");
        expect(metrics.physical_width == 1280 && metrics.physical_height == 720,
               "default physical size");
        expect(metrics.dpi_scale == 1.0f && metrics.focused, "default scale/focus");
        expect(window.queued_events() == 0, "no events before the script pushes one");
        expect(window.redraw_requests() == 0, "no redraw before it is requested");
    }

    // Resize normalizes logical/physical pixels and queues one Resize event with the new metrics.
    {
        FakeWindow window;
        window.resize(1024, 768, 1.5);
        const WindowMetrics metrics = window.metrics();
        expect(metrics.logical_width == 1024 && metrics.logical_height == 768, "resize logical");
        expect(metrics.physical_width == 1536 && metrics.physical_height == 1152,
               "resize physical follows the DPI scale");
        expect(metrics.dpi_scale == 1.5f, "resize scale");
        expect(window.queued_events() == 1 && window.has_queued(WindowEventType::Resize),
               "resize event queued");
        std::array<WindowEvent, 1> events;
        expect(window.poll_events(events) == 1, "resize polled");
        expect(events[0].metrics.logical_width == 1024, "event carries the new metrics");
        expect(window.queued_events() == 0, "event dequeued");
        window.resize(0, 100);
        expect(window.metrics().logical_width == 1024, "invalid resize ignored");
    }

    // Events are delivered oldest first and survive a smaller poll buffer.
    {
        FakeWindow window;
        window.resize(800, 600);
        window.set_dpi_scale(2.0);
        window.set_focused(false);
        std::array<WindowEvent, 1> events;
        expect(window.poll_events(events) == 1 && events[0].type == WindowEventType::Resize,
               "oldest event first");
        expect(window.poll_events(events) == 1 && events[0].type == WindowEventType::DpiChange,
               "second event next");
        expect(window.poll_events(events) == 1 && events[0].type == WindowEventType::Blur,
               "focus loss queued");
        expect(!window.metrics().focused, "metrics reflect blur");
        expect(!window.wait_for_events(0.0), "wait reports no queued event without sleeping");
        expect(window.poll_events(events) == 0, "empty poll");
    }

    // Occlusion/minimize/restore are normalized lifecycle events, not OS messages.
    {
        FakeWindow window;
        window.set_occluded(true);
        window.set_minimized(true);
        window.set_minimized(false);
        window.set_occluded(false);
        std::array<WindowEvent, 4> events;
        expect(window.poll_events(events) == 4, "four lifecycle events");
        expect(events[0].type == WindowEventType::Occluded, "occluded");
        expect(events[1].type == WindowEventType::Minimized, "minimized");
        expect(events[2].type == WindowEventType::Restored, "restored from minimize");
        expect(events[3].type == WindowEventType::Restored, "restored from occlusion");
        window.set_occluded(false);
        expect(window.queued_events() == 0, "redundant lifecycle change is a no-op");
    }

    // The event queue is bounded: the oldest event is dropped and counted.
    {
        FakeWindow window;
        for (std::size_t index = 0; index < kMaxFakeWindowEvents + 5; ++index) {
            window.push_event(WindowEventType::Restored);
        }
        expect(window.queued_events() == kMaxFakeWindowEvents, "event queue capped");
        expect(window.dropped_events() == 5, "dropped events counted");
        window.close();
        window.close();
        expect(window.queued_events() == kMaxFakeWindowEvents, "close is coalesced and capped");
        expect(window.closed(), "closed flag set");
    }

    // Show/hide and redraw requests are recorded for assertions.
    {
        FakeWindow window;
        window.hide();
        expect(!window.visible(), "hide recorded");
        window.show();
        expect(window.visible(), "show recorded");
        window.request_redraw();
        window.request_redraw();
        expect(window.redraw_requests() == 2, "redraw requests counted");
        expect(window.native_handle().window == nullptr, "no native handle");
    }

    // Exact metrics injection leaves the event queue alone (for scripts that only change size).
    {
        FakeWindow window;
        WindowMetrics metrics = default_fake_metrics();
        metrics.insets.top = 24.0f;
        window.set_metrics(metrics);
        expect(window.metrics().insets.top == 24.0f, "insets round-trip");
        expect(window.queued_events() == 0, "set_metrics does not queue an event");
    }

    if (failures != 0) {
        std::cerr << "fake_hal: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
