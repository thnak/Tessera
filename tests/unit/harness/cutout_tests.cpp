// SPDX-License-Identifier: MIT

// CutoutInfo (issue #222, docs/specs/os-events-v1.md section 2): the default reports none, a fake
// injects one, several and too many cutouts (extras dropped), SafeAreaChanged coalesces in the
// fake and in the platform event ring, and the metrics dump lists the rectangles.

#include "fake_hal.hpp"
#include "tessera/hal/cutout.hpp"
#include "window_event_queue.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace tessera;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cutout: " << message << "\n";
        ++failures;
    }
    return condition;
}

} // namespace

int main() {
    {
        const hal::WindowMetrics defaults;
        expect(defaults.cutouts.count == 0, "default metrics report no cutouts");
        expect(defaults.cutouts.waterfall_insets.left == 0.0f, "default waterfall is zero");
        FakeWindow window;
        expect(window.metrics().cutouts.count == 0, "fake window reports no cutouts");
    }

    // One cutout: a top-center punch-hole.
    {
        FakeWindow window;
        const std::array<hal::WindowRect, 1> hole{hal::WindowRect{190, 0, 20, 20}};
        window.set_safe_area({0, 24, 0, 0}, hole);
        const hal::WindowMetrics metrics = window.metrics();
        expect(metrics.cutouts.count == 1, "one cutout is kept");
        expect(metrics.cutouts.bounds[0].x == 190 && metrics.cutouts.bounds[0].width == 20,
               "the rectangle is kept verbatim");
        expect(metrics.insets.top == 24, "insets change with the cutouts");
        expect(window.has_queued(hal::WindowEventType::SafeAreaChanged),
               "SafeAreaChanged is queued");
    }

    // Several cutouts keep their order.
    {
        const std::array<hal::WindowRect, 3> rects{hal::WindowRect{0, 0, 10, 10},
                                                   hal::WindowRect{20, 0, 10, 10},
                                                   hal::WindowRect{40, 0, 10, 10}};
        const hal::CutoutInfo info = hal::make_cutout_info(rects);
        expect(info.count == 3 && info.bounds[2].x == 40, "several cutouts keep their order");
    }

    // More than kMaxCutouts: the extras are dropped, nothing is read past the array.
    {
        std::array<hal::WindowRect, hal::kMaxCutouts + 3> rects{};
        for (std::size_t i = 0; i < rects.size(); ++i) {
            rects[i] = hal::WindowRect{static_cast<float>(i * 10), 0, 5, 5};
        }
        const hal::CutoutInfo info = hal::make_cutout_info(rects);
        expect(info.count == hal::kMaxCutouts, "the count is capped at kMaxCutouts");
        expect(info.bounds[hal::kMaxCutouts - 1].x == (hal::kMaxCutouts - 1) * 10.0f,
               "the first kMaxCutouts are the ones kept");
    }

    // Empty, non-finite and zero-area rectangles are skipped and do not use a slot.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::array<hal::WindowRect, 4> rects{
            hal::WindowRect{0, 0, 0, 10}, hal::WindowRect{nan, 0, 10, 10},
            hal::WindowRect{5, 5, 10, 10}, hal::WindowRect{0, 0, -1, 4}};
        const hal::CutoutInfo info = hal::make_cutout_info(rects);
        expect(info.count == 1 && info.bounds[0].x == 5, "unusable rectangles are skipped");
        expect(hal::make_cutout_info({}).count == 0, "an empty list reports none");
    }

    // Coalescing in the fake: two changes leave one event carrying the newest metrics.
    {
        FakeWindow window;
        const std::array<hal::WindowRect, 1> a{hal::WindowRect{0, 0, 10, 10}};
        const std::array<hal::WindowRect, 2> b{hal::WindowRect{0, 0, 10, 10},
                                               hal::WindowRect{30, 0, 10, 10}};
        window.set_safe_area({}, a);
        window.set_safe_area({0, 30, 0, 0}, b);
        expect(window.queued_events() == 1, "consecutive SafeAreaChanged coalesce in the fake");
        std::array<hal::WindowEvent, 4> out{};
        expect(window.poll_events(out) == 1 && out[0].metrics.cutouts.count == 2 &&
                   out[0].metrics.insets.top == 30,
               "the surviving event carries the newest metrics");
    }

    // Coalescing in the platform ring; another event in between breaks the run.
    {
        hal::detail::EventQueue queue;
        hal::WindowEvent event;
        event.type = hal::WindowEventType::SafeAreaChanged;
        event.metrics.insets.top = 1;
        queue.push(event);
        event.metrics.insets.top = 2;
        queue.push(event);
        expect(queue.count() == 1, "SafeAreaChanged coalesces in the platform ring");
        hal::WindowEvent focus;
        focus.type = hal::WindowEventType::Focus;
        queue.push(focus);
        event.metrics.insets.top = 3;
        queue.push(event);
        expect(queue.count() == 3, "a different event in between keeps both");
        std::array<hal::WindowEvent, 4> out{};
        queue.copy_to(out);
        expect(out[0].metrics.insets.top == 2, "the coalesced event keeps the newest metrics");
    }

    // The dump lists the rectangles and is deterministic.
    {
        hal::WindowMetrics metrics;
        metrics.logical_width = 400;
        metrics.logical_height = 800;
        const std::array<hal::WindowRect, 2> rects{hal::WindowRect{190, 0, 20, 20},
                                                   hal::WindowRect{300, 0, 8, 8}};
        metrics.cutouts = hal::make_cutout_info(rects, {2, 0, 2, 0});
        const std::string dump = hal::format_window_metrics(metrics);
        expect(dump.find("cutouts 2\n") != std::string::npos, "the dump has the count");
        expect(dump.find("cutout 190 0 20 20\n") != std::string::npos, "the dump lists rect one");
        expect(dump.find("cutout 300 0 8 8\n") != std::string::npos, "the dump lists rect two");
        expect(dump.find("waterfall 2 0 2 0\n") != std::string::npos, "the dump has the waterfall");
        expect(dump == hal::format_window_metrics(metrics), "the dump is deterministic");
        expect(hal::format_window_metrics(hal::WindowMetrics{}).find("cutouts 0\n") !=
                   std::string::npos,
               "the default dump reports no cutouts");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "cutout: ok\n";
    return 0;
}
