// SPDX-License-Identifier: MIT
//
// Full-screen and caption regions through the fake window (#232): the `hal::Window` seam as the app
// driver and the headless harness see it. A real platform resolves full-screen asynchronously
// through the window manager; the fake resolves it synchronously and reports the same normalized
// values, so the request/refusal contract and the metrics a caller reads are covered without a
// display.

#include "fake_hal.hpp"
#include "tessera/hal/cutout.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

using namespace tessera;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_fullscreen_fake: " << message << "\n";
        ++failures;
    }
    return condition;
}

} // namespace

int main() {
    // A fresh window supports full-screen and reports the windowed mode.
    {
        FakeWindow window;
        expect(window.supports_fullscreen(), "the fake supports full-screen by default");
        expect(!window.metrics().fullscreen, "the default windowed mode is reported");
        expect(window.metrics().caption_region_count == 0,
               "the default metrics carry no caption regions");
    }

    // Round trip: enter reports full-screen and queues one Resize; leaving reports windowed again.
    {
        FakeWindow window;
        const std::uint32_t width = window.metrics().logical_width;
        window.set_fullscreen(true);
        expect(window.metrics().fullscreen, "entering reports the full-screen mode");
        expect(window.has_queued(hal::WindowEventType::Resize),
               "entering reports a Resize so the engine repaints at the new size");
        expect(window.metrics().logical_width == width,
               "the fake does not invent a size a real display owns");

        (void)window.poll_events({});
        window.set_fullscreen(false);
        expect(!window.metrics().fullscreen, "leaving reports the windowed mode again");
        expect(window.has_queued(hal::WindowEventType::Resize),
               "leaving reports a Resize so the engine repaints at the restored size");
    }

    // A repeated request is a no-op: it neither flips the mode nor queues a second event.
    {
        FakeWindow window;
        window.set_fullscreen(true);
        (void)window.poll_events({});
        const std::size_t queued_before = window.queued_events();
        window.set_fullscreen(true);
        expect(window.metrics().fullscreen, "a repeated enter keeps the mode");
        expect(window.queued_events() == queued_before, "a repeated enter queues no extra event");
        window.set_fullscreen(false);
        (void)window.poll_events({});
        const std::size_t queued_after_exit = window.queued_events();
        window.set_fullscreen(false);
        expect(!window.metrics().fullscreen, "a repeated exit keeps the windowed mode");
        expect(window.queued_events() == queued_after_exit,
               "a repeated exit queues no extra event");
    }

    // A platform with no window manager refuses the request and leaves the mode alone, which is
    // what the app's unsupported path decides from.
    {
        FakeWindow window;
        window.set_fullscreen_supported(false);
        expect(!window.supports_fullscreen(), "the scripted capability is reported");
        window.set_fullscreen(true);
        expect(!window.metrics().fullscreen,
               "a refused request leaves the windowed mode (the resolved state, not the request)");
        expect(!window.has_queued(hal::WindowEventType::Resize),
               "a refused request reports no resize");
    }

    // Caption regions: publishing them reports them through the metrics, an empty list clears them,
    // and the cap comes from the shared builder.
    {
        FakeWindow window;
        const std::array<hal::CaptionRegion, 2> regions{
            hal::CaptionRegion{hal::WindowRect{0, 0, 32, 32}, hal::CaptionRegionKind::SystemMenu},
            hal::CaptionRegion{hal::WindowRect{200, 0, 40, 32}, hal::CaptionRegionKind::Close},
        };
        window.set_caption_regions(regions);
        const hal::WindowMetrics published = window.metrics();
        expect(published.caption_region_count == 2, "both regions are published");
        expect(published.caption_regions[0].kind == hal::CaptionRegionKind::SystemMenu,
               "the first region keeps its order and kind");
        expect(published.caption_regions[1].bounds.width == 40.0f,
               "the second region keeps its rectangle");

        // The engine resolves a point through the same helper the platform uses.
        const auto view = std::span<const hal::CaptionRegion>(published.caption_regions.data(),
                                                              published.caption_region_count);
        expect(hal::caption_region_at(view, 20.0f, 10.0f) != nullptr,
               "a point inside a published region resolves");
        expect(hal::caption_region_at(view, 100.0f, 10.0f) == nullptr,
               "a gap between regions is client area");

        window.set_caption_regions({});
        expect(window.metrics().caption_region_count == 0,
               "an empty list returns the window to the system-chrome answer");
    }

    // The unsupported refusals are also visible through the deterministic metrics dump, so a
    // harness script can assert the mode without a display.
    {
        FakeWindow window;
        expect(hal::format_window_metrics(window.metrics()).find("fullscreen 0\n") !=
                   std::string::npos,
               "the metrics dump carries the windowed mode");
        window.set_fullscreen(true);
        expect(hal::format_window_metrics(window.metrics()).find("fullscreen 1\n") !=
                   std::string::npos,
               "the metrics dump carries the full-screen mode");
    }

    if (failures == 0) {
        std::cout << "window_fullscreen_fake: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
