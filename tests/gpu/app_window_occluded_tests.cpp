// SPDX-License-Identifier: MIT
//
// Occluded/minimized pause on the window path: a hidden window + Vulkan swapchain driven by the
// deterministic single-iteration seam WindowHost::pump_once(), with metrics snapshots reporting the
// lifecycle (occlusion has no Win32/X11 event yet). Proves a fresh host paints; an occluded or
// minimized snapshot acquires, paints and presents nothing while still pumping input; the resume
// edge repaints the full target; and a visible-but-unfocused window keeps painting. Skips when the
// platform has no window backend or presentation surface.

#include "tessera/app/host.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::AppHost;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::hal::WindowMetrics;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_occluded: " << message << "\n";
    }
    return condition;
}

// A complete snapshot at the window's current size: apply_metrics takes the whole lifecycle state,
// so a partial one would claim a different size/scale and exercise the resize path instead of the
// pause. Reading it back from the host keeps a high-DPI display from making the metrics disagree
// with the real surface.
WindowMetrics snapshot(WindowHost& host, bool focused, bool occluded, bool minimized) {
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
    metrics.minimized = minimized;
    return metrics;
}

void set_label(Node& label, const char* text) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    label.set_paint_data(std::move(data));
}

InputEvent pointer_move(float x, float y) {
    InputEvent event;
    event.type = InputEventType::PointerMove;
    event.x = x;
    event.y = y;
    event.pointer_id = 0;
    event.pointer_kind = tessera::ui::PointerKind::Mouse;
    return event;
}

// One presenting iteration, the way `run()` performs it.
//
// `render_and_present()` skips a frame whose target slot still has GPU work in flight
// (src/app/window.cpp, the `in_flight[slot]->is_complete()` gate) rather than waiting on the GPU,
// so a single `pump_once()` can legitimately return false for a busy slot. `run()` handles that by
// latching a full repaint and retrying after a bounded idle wait; this helper does the same, so the
// assertions below stay about pause/resume semantics instead of GPU slot recycling (issue #319).
// The bounded attempt count fails the assertion on a machine that never frees a slot, rather than
// hanging.
bool pump_until_presented(WindowHost& host) {
    constexpr int kMaxAttempts = 200;
    constexpr auto kRetryPause = std::chrono::milliseconds(2);
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (host.pump_once()) {
            return true;
        }
        std::this_thread::sleep_for(kRetryPause);
    }
    return false;
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    set_label(label, "Occluded frame");

    tessera::app::WindowOptions options;
    options.title = "Tessera occluded pause";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;

    WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout << "app_window_occluded: skipped (no window backend or presentation surface)\n";
        return 0;
    }

    // 1. A fresh host paints: the first bounded iteration lays out, paints and presents one frame.
    if (!expect(pump_until_presented(host),
                "a fresh host presents a frame on its first pump_once")) {
        return 1;
    }
    const WindowFrameStats& stats = host.stats();
    if (!expect(stats.frames >= 1, "the first iteration painted a frame") ||
        !expect(stats.presented >= 1, "the first iteration presented a frame") ||
        !expect(stats.paused_iterations == 0, "a visible window is not paused")) {
        return 1;
    }

    // 2. An occluded snapshot pauses: no acquire, no paint, no present, but input still drains and
    //    the paused iteration is counted.
    const std::uint32_t presented_before = stats.presented;
    const std::uint32_t frames_before = stats.frames;
    const std::uint32_t paused_before = stats.paused_iterations;
    host.apply_metrics(snapshot(host, true, true, false));
    if (!expect(!host.pump_once(), "an occluded window presents nothing") ||
        !expect(!host.pump_once(), "a second paused iteration presents nothing") ||
        !expect(stats.presented == presented_before, "no frame was presented while occluded") ||
        !expect(stats.frames == frames_before, "no frame was painted while occluded") ||
        !expect(stats.paused_iterations >= paused_before + 2, "paused iterations are counted")) {
        return 2;
    }

    // The loop is not deaf while paused: a queued input event is still drained (and wakes it).
    const std::uint32_t input_before = stats.input_events;
    host.post_input(pointer_move(8.0f, 8.0f));
    if (!expect(!host.pump_once(), "input does not unpause an occluded window") ||
        !expect(stats.input_events > input_before, "a paused iteration still pumps input") ||
        !expect(stats.frames == frames_before, "the paused input iteration painted nothing")) {
        return 2;
    }

    // 3. The resume edge presents again and the frame is a full repaint (the plan covers the whole
    //    target), not an incremental one derived from pre-pause damage.
    const std::uint32_t full_before = stats.full_repaints;
    host.apply_metrics(snapshot(host, true, false, false));
    if (!expect(pump_until_presented(host), "the visible resume iteration presents")) {
        return 3;
    }
    if (!expect(stats.presented == presented_before + 1,
                "the resume presented exactly one frame") ||
        !expect(stats.full_repaints > full_before, "the resume frame was a full repaint") ||
        !expect(host.host().last_render().full_redraw,
                "the pipeline planned a full redraw on resume")) {
        return 3;
    }

    // 4. Minimized behaves like occluded, and clearing both flags resumes.
    const std::uint32_t paused_after_resume = stats.paused_iterations;
    const std::uint32_t presented_after_resume = stats.presented;
    host.apply_metrics(snapshot(host, true, false, true));
    if (!expect(!host.pump_once(), "a minimized window presents nothing") ||
        !expect(stats.presented == presented_after_resume,
                "no frame was presented while minimized") ||
        !expect(stats.paused_iterations == paused_after_resume + 1,
                "the minimized iteration is counted as paused")) {
        return 4;
    }
    host.apply_metrics(snapshot(host, true, false, false));
    if (!expect(pump_until_presented(host), "clearing minimized resumes presentation") ||
        !expect(stats.presented == presented_after_resume + 1, "the resume presented one frame") ||
        !expect(host.host().last_render().full_redraw,
                "the minimized resume repainted the full target")) {
        return 4;
    }

    // 5. Focus alone never pauses. There is no seam that injects a platform Focus/Blur event
    // without
    //    a real OS focus change, so the unfocused state comes through the metrics snapshot; a real
    //    scene change proves the iteration was free to paint rather than merely damage-free.
    set_label(label, "Unfocused frame");
    const std::uint32_t paused_before_blur = stats.paused_iterations;
    const std::uint32_t presented_before_blur = stats.presented;
    host.apply_metrics(snapshot(host, false, false, false));
    if (!expect(pump_until_presented(host), "a visible but unfocused window still paints") ||
        !expect(stats.presented == presented_before_blur + 1, "the blurred frame was presented") ||
        !expect(stats.paused_iterations == paused_before_blur, "blur alone never pauses")) {
        return 5;
    }

    // 6. pump_once() is one iteration, not a forced-frame loop: a clean scene and no pending input
    //    present nothing, so a pause is never confused with the quiet path.
    if (!expect(!host.pump_once(), "an unchanged scene presents nothing on the next iteration")) {
        return 6;
    }

    std::cout << "app_window_occluded: ok (frames=" << stats.frames
              << " presented=" << stats.presented << " full_repaints=" << stats.full_repaints
              << " paused_iterations=" << stats.paused_iterations
              << " input_events=" << stats.input_events << ")\n";
    return 0;
}
