// SPDX-License-Identifier: MIT
//
// Host-owned caret blink on the window path: a hidden window + Vulkan swapchain driven by the
// deterministic single-iteration seam WindowHost::pump_once(), with a ManualClock injected through
// WindowOptions::clock. Proves an activated caret presents once and is visible; a blink flip just
// past each half-period presents exactly one frame (visible -> hidden -> visible) and bumps the
// caret_blinks counter; an inactive (unfocused) caret requests nothing and stays idle across any
// number of clock advances; and a paused (occluded) window never ticks the caret, resuming it after
// the restore. Deterministic: no wall clock, no sleeps, no real OS focus/occlusion change.
// Skips when the platform has no window backend or presentation surface.

#include "tessera/app/host.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::AppHost;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::hal::WindowMetrics;
using tessera::runtime::ManualClock;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

// Half the default 500/500 ms caret period, plus a step that cannot land exactly on a phase
// boundary (the visible window is half-open, so 0.5 would already be hidden).
constexpr double kHalfPeriod = 0.5001;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_caret: " << message << "\n";
    }
    return condition;
}

// A complete snapshot at the window's current size: apply_metrics takes the whole lifecycle state,
// so a partial one would exercise the resize path instead of the pause.
WindowMetrics snapshot(WindowHost& host, bool focused, bool occluded) {
    const AppHost& current = host.host();
    WindowMetrics metrics;
    metrics.logical_width = static_cast<std::uint32_t>(current.width());
    metrics.logical_height = static_cast<std::uint32_t>(current.height());
    const auto extent = current.target_extent();
    metrics.physical_width = extent.width;
    metrics.physical_height = extent.height;
    metrics.dpi_scale = current.dpi_scale();
    metrics.focused = focused;
    metrics.occluded = occluded;
    metrics.minimized = false;
    return metrics;
}

void set_label(Node& label, const char* text) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    label.set_paint_data(std::move(data));
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    set_label(label, "Caret blink");

    ManualClock clock;
    tessera::app::WindowOptions options;
    options.title = "Tessera caret blink";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.clock = &clock;

    WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout << "app_window_caret: skipped (no window backend or presentation surface)\n";
        return 0;
    }
    const WindowFrameStats& stats = host.stats();

    // 0. A fresh host owns no active caret: nothing will tick or request frames for it until the
    //    app grants focus.
    if (!expect(!host.caret_visible(), "a fresh host has no visible caret")) {
        return 1;
    }

    // 1. Activating shows the caret immediately and the first iteration presents the startup frame.
    host.set_caret_active(true);
    if (!expect(host.caret_visible(), "activating shows the caret immediately") ||
        !expect(host.pump_once(), "the activation iteration presents the startup frame")) {
        return 1;
    }
    if (!expect(stats.caret_blinks == 0, "a policy change is not a blink flip")) {
        return 1;
    }
    std::uint32_t presented = stats.presented;
    if (!expect(!host.pump_once(), "a second iteration at the same clock time presents nothing")) {
        return 1;
    }

    // 1a. Just past the visible half-period: exactly one flip, one frame, caret hidden.
    clock.advance(kHalfPeriod);
    if (!expect(host.pump_once(), "the visible -> hidden flip presents a frame")) {
        return 1;
    }
    if (!expect(!host.caret_visible(), "the caret is hidden after the visible half-period") ||
        !expect(stats.caret_blinks == 1, "exactly one flip was counted") ||
        !expect(stats.presented == presented + 1, "the flip presented exactly one frame") ||
        !expect(!host.pump_once(), "a second iteration at the same time presents nothing again")) {
        return 1;
    }

    // 1b. Just past the period boundary: visible again, one flip, one frame.
    presented = stats.presented;
    clock.advance(kHalfPeriod);
    if (!expect(host.pump_once(), "the hidden -> visible flip presents a frame") ||
        !expect(host.caret_visible(), "the caret is visible after the period boundary") ||
        !expect(stats.caret_blinks == 2, "the second flip was counted") ||
        !expect(stats.presented == presented + 1, "the second flip presented exactly one frame") ||
        !expect(!host.pump_once(), "the second flip left no frame pending")) {
        return 1;
    }

    // 1c. One more half-period keeps the cadence: one flip per half-period, never two.
    presented = stats.presented;
    clock.advance(kHalfPeriod);
    if (!expect(host.pump_once(), "the third half-period presents") ||
        !expect(!host.caret_visible(), "the third half-period is hidden") ||
        !expect(stats.caret_blinks == 3, "the third flip was counted") ||
        !expect(stats.presented == presented + 1, "the third flip presented exactly one frame")) {
        return 1;
    }

    // 2. Unfocusing hides the caret and requests nothing: no frame, and advancing the clock over
    //    several whole periods never presents or ticks.
    const std::uint32_t inactive_presented = stats.presented;
    const std::uint32_t inactive_blinks = stats.caret_blinks;
    const std::uint32_t inactive_frames = stats.frames;
    host.set_caret_active(false);
    if (!expect(!host.caret_visible(), "deactivating hides the caret immediately") ||
        !expect(!host.pump_once(), "the iteration after deactivation presents nothing")) {
        return 2;
    }
    for (int step = 0; step < 4; ++step) {
        clock.advance(0.5);
        if (!expect(!host.pump_once(), "an inactive caret presents nothing across the clock")) {
            return 2;
        }
    }
    if (!expect(stats.presented == inactive_presented, "deactivation requested no frame") ||
        !expect(stats.frames == inactive_frames, "no frame was even begun while inactive") ||
        !expect(stats.caret_blinks == inactive_blinks, "an inactive caret is never ticked") ||
        !expect(!host.caret_visible(), "the caret stays hidden while inactive")) {
        return 2;
    }

    // 2b. Reactivating shows the caret, presents exactly once and resumes ticking from that
    // instant.
    host.set_caret_active(true);
    if (!expect(host.caret_visible(), "reactivating shows the caret immediately") ||
        !expect(host.pump_once(), "reactivating presents one frame") ||
        !expect(stats.presented == inactive_presented + 1,
                "reactivation presented exactly one frame") ||
        !expect(!host.pump_once(), "the reactivation left no frame pending")) {
        return 2;
    }
    presented = stats.presented;
    clock.advance(kHalfPeriod);
    if (!expect(host.pump_once(), "ticking resumed after reactivation") ||
        !expect(!host.caret_visible(), "the resumed cycle hides after the half-period") ||
        !expect(stats.presented == presented + 1, "the resumed flip presented one frame")) {
        return 2;
    }

    // 3. A paused (occluded) window never ticks the caret: pump_once() returns false, no frame and
    //    no flip, even when the clock crosses a phase boundary. The pause wins over the blink.
    host.apply_metrics(snapshot(host, false, true));
    const std::uint32_t paused_presented = stats.presented;
    const std::uint32_t paused_blinks = stats.caret_blinks;
    const std::uint32_t paused_frames = stats.frames;
    const std::uint32_t paused_iterations = stats.paused_iterations;
    const bool frozen_visible = host.caret_visible();
    clock.advance(kHalfPeriod);
    if (!expect(!host.pump_once(), "a paused window presents nothing") ||
        !expect(stats.presented == paused_presented, "no frame was presented while paused") ||
        !expect(stats.frames == paused_frames, "no frame was begun while paused") ||
        !expect(stats.caret_blinks == paused_blinks, "a paused window does not tick the caret") ||
        !expect(host.caret_visible() == frozen_visible, "the caret state is frozen while paused") ||
        !expect(stats.paused_iterations == paused_iterations + 1,
                "the iteration counted as paused")) {
        return 3;
    }
    // A second paused iteration a whole period later lands in the same phase: still no work at all.
    clock.advance(1.0);
    if (!expect(!host.pump_once(), "a second paused iteration presents nothing") ||
        !expect(stats.caret_blinks == paused_blinks, "the pause held across a full period")) {
        return 3;
    }

    // 3b. Resume: the next iteration presents (the resume repaint) and the caret catches up with
    //     exactly one flip for the elapsed time - a jump flips at most once.
    host.apply_metrics(snapshot(host, true, false));
    const bool pre_resume_visible = host.caret_visible();
    if (!expect(host.pump_once(), "the resume iteration presents")) {
        return 3;
    }
    if (!expect(stats.caret_blinks == paused_blinks + 1,
                "the resume tick flipped the caret exactly once") ||
        !expect(host.caret_visible() != pre_resume_visible,
                "the caught-up tick changed the caret visibility") ||
        !expect(stats.presented == paused_presented + 1, "the resume presented one frame")) {
        return 3;
    }

    // 3c. Ticking stays live after the resume: the next half-period flips and presents once.
    presented = stats.presented;
    clock.advance(kHalfPeriod);
    if (!expect(host.pump_once(), "the caret keeps ticking after the resume") ||
        !expect(stats.presented == presented + 1, "the post-resume flip presented one frame")) {
        return 3;
    }

    std::cout << "app_window_caret: ok (presented=" << stats.presented
              << " caret_blinks=" << stats.caret_blinks << " frames=" << stats.frames
              << " paused_iterations=" << stats.paused_iterations << ")\n";
    return 0;
}
