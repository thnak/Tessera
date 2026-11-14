// SPDX-License-Identifier: MIT

// SafeArea inset arithmetic (issue #223, docs/specs/safe-area-v1.md sections 3, 4 and 9): edges,
// sources, minimum, logical start/end under right-to-left, cutout and hinge insets, and the four
// scripted devices (notched phone, gesture-navigation phone, tablet keyboard, book-mode foldable).

#include "fake_hal.hpp"
#include "tessera/hal/safe_area.hpp"
#include "window_event_queue.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using namespace tessera;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "safe_area: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool same(const hal::WindowInsets& got, float left, float top, float right, float bottom) {
    return got.left == left && got.top == top && got.right == right && got.bottom == bottom;
}

} // namespace

int main() {
    // Defaults: system bars, cutout, IME and hinge count; all four edges; no floor.
    {
        FakeWindow window;
        window.resize(400, 800);
        window.set_safe_area({0, 24, 0, 48}, {});
        const hal::SafeAreaConfig config;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 0, 24, 0, 48),
               "system bars apply on every edge by default");
    }

    // A notched phone: a top-center cutout inside a taller status bar, a rounded-corner waterfall.
    {
        FakeWindow window;
        window.resize(400, 800);
        const std::array<hal::WindowRect, 1> notch{hal::WindowRect{150, 0, 100, 44}};
        window.set_safe_area({0, 20, 0, 0}, notch);
        const hal::WindowMetrics metrics = window.metrics();
        expect(same(hal::cutout_insets(metrics), 0, 44, 0, 0),
               "a top-center notch is a top inset only");
        expect(same(hal::resolve_safe_area(metrics, {}), 0, 44, 0, 0),
               "the larger of bar and cutout wins, they do not add");
        hal::SafeAreaConfig bars_only;
        bars_only.sources = hal::kSourceSystemBars;
        expect(same(hal::resolve_safe_area(metrics, bars_only), 0, 20, 0, 0),
               "without the cutout source only the bar counts");
    }

    // A cutout that touches no edge imposes nothing; one on the left edge (landscape) imposes left.
    {
        FakeWindow window;
        window.resize(400, 800);
        const std::array<hal::WindowRect, 1> floating{hal::WindowRect{100, 100, 20, 20}};
        window.set_safe_area({}, floating);
        expect(same(hal::cutout_insets(window.metrics()), 0, 0, 0, 0),
               "a floating rectangle is not an inset");
        const std::array<hal::WindowRect, 1> side{hal::WindowRect{0, 300, 40, 100}};
        window.set_safe_area({}, side);
        expect(same(hal::cutout_insets(window.metrics()), 40, 0, 0, 0),
               "a left-edge cutout is a left inset");
    }

    // Gesture navigation: the gesture zone only counts when the source is on.
    {
        FakeWindow window;
        window.resize(400, 800);
        window.set_ime_insets({}, {24, 0, 24, 20});
        const hal::WindowMetrics metrics = window.metrics();
        expect(same(hal::resolve_safe_area(metrics, {}), 0, 0, 0, 0),
               "gestures are off by default");
        hal::SafeAreaConfig with;
        with.sources = hal::kSourcesDefault | hal::kSourceGestures;
        expect(same(hal::resolve_safe_area(metrics, with), 24, 0, 24, 20),
               "gestures count when asked for");
    }

    // Tablet keyboard: the IME inset includes the bar under it, so the bottom is the max.
    {
        FakeWindow window;
        window.resize(800, 600);
        window.set_safe_area({0, 0, 0, 48}, {});
        window.set_ime_insets({0, 0, 0, 320});
        const hal::WindowMetrics metrics = window.metrics();
        expect(same(hal::resolve_safe_area(metrics, {}), 0, 0, 0, 320),
               "the keyboard raises the bottom inset to its height");
        hal::SafeAreaConfig no_ime;
        no_ime.sources = hal::kSourceSystemBars;
        expect(same(hal::resolve_safe_area(metrics, no_ime), 0, 0, 0, 48),
               "without the ime source the bar remains");
        expect(window.has_queued(hal::WindowEventType::ImeChanged), "ImeChanged is queued");
    }

    // Edges select sides; an unselected side is edge to edge even with a minimum.
    {
        FakeWindow window;
        window.resize(400, 800);
        window.set_safe_area({10, 24, 12, 48}, {});
        hal::SafeAreaConfig config;
        config.edges = hal::kEdgeTop | hal::kEdgeBottom;
        config.minimum = 16;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 0, 24, 0, 48),
               "unselected sides are zero, selected ones keep their inset");
        FakeWindow flat;
        flat.resize(400, 800);
        expect(same(hal::resolve_safe_area(flat.metrics(), config), 0, 16, 0, 16),
               "the minimum floors a selected side on a device with no insets");
    }

    // Logical start and end swap under right-to-left.
    {
        FakeWindow window;
        window.resize(400, 800);
        window.set_safe_area({30, 0, 5, 0}, {});
        hal::SafeAreaConfig config;
        config.edges = hal::kEdgeStart;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 30, 0, 0, 0),
               "start is left in left-to-right");
        config.rtl = true;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 0, 0, 5, 0),
               "start is right in right-to-left");
        config.edges = hal::kEdgeEnd;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 30, 0, 0, 0),
               "end is left in right-to-left");
    }

    // Book-mode foldable: a full-occlusion vertical hinge splits the window; a single pane takes
    // the larger side.
    {
        FakeWindow window;
        window.resize(1000, 700);
        hal::FoldInfo fold;
        fold.state = hal::FoldState::HalfOpened;
        fold.orientation = hal::FoldOrientation::Vertical;
        fold.occlusion = hal::FoldOcclusion::Full;
        fold.separating = true;
        fold.hinge_bounds = {480, 0, 40, 700};
        window.set_fold(fold);
        expect(window.has_queued(hal::WindowEventType::FoldChanged), "FoldChanged is queued");
        expect(same(hal::resolve_safe_area(window.metrics(), {}), 0, 0, 520, 0),
               "an equal split keeps the left pane");
        fold.hinge_bounds = {300, 0, 40, 700};
        window.set_fold(fold);
        expect(same(hal::resolve_safe_area(window.metrics(), {}), 340, 0, 0, 0),
               "the larger right side is kept");
        hal::SafeAreaConfig no_hinge;
        no_hinge.sources = hal::kSourceSystemBars;
        expect(same(hal::resolve_safe_area(window.metrics(), no_hinge), 0, 0, 0, 0),
               "the hinge only counts as a source");
        fold.occlusion = hal::FoldOcclusion::None;
        window.set_fold(fold);
        expect(same(hal::resolve_safe_area(window.metrics(), {}), 0, 0, 0, 0),
               "a non-occluding fold imposes nothing");
        fold.occlusion = hal::FoldOcclusion::Full;
        fold.orientation = hal::FoldOrientation::Horizontal;
        fold.hinge_bounds = {0, 200, 1000, 30};
        window.set_fold(fold);
        expect(same(hal::resolve_safe_area(window.metrics(), {}), 0, 230, 0, 0),
               "a horizontal hinge near the top keeps the larger bottom pane");
    }

    // Hostile values count as zero and never poison the result.
    {
        FakeWindow window;
        window.resize(400, 800);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        window.set_safe_area({nan, -5, std::numeric_limits<float>::infinity(), 10}, {});
        expect(same(hal::resolve_safe_area(window.metrics(), {}), 0, 0, 0, 10),
               "non-finite and negative insets are zero");
        hal::SafeAreaConfig config;
        config.minimum = nan;
        expect(same(hal::resolve_safe_area(window.metrics(), config), 0, 0, 0, 10),
               "a non-finite minimum is zero");
    }

    // The event ring coalesces the new events like SafeAreaChanged.
    {
        hal::detail::EventQueue queue;
        hal::WindowEvent event;
        event.type = hal::WindowEventType::ImeChanged;
        queue.push(event);
        queue.push(event);
        event.type = hal::WindowEventType::FoldChanged;
        queue.push(event);
        queue.push(event);
        expect(queue.count() == 2, "consecutive Ime and Fold events coalesce into the newest");
    }

    return failures == 0 ? 0 : 1;
}
