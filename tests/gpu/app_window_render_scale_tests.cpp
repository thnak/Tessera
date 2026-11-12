// SPDX-License-Identifier: MIT
//
// The adaptive render-scale consumer on the real window path (docs/design/performance.md
// section 6.1, docs/specs/runtime-config-v1.md section 6.2). A hidden window plus a Vulkan
// swapchain is driven through the deterministic single-iteration seam WindowHost::pump_once():
//
//  1. at the default renderScale of 1.0 the frame renders straight into the swapchain image: the
//     target extent equals the presented extent, no offscreen target exists and no resample runs -
//     the unchanged full-quality path (the pixel guarantee itself is pinned by the unchanged
//     app_window_capture/frame-pipeline goldens in the suite);
//  2. four over-budget observations move the controller one ladder step down; the next frame
//  renders
//     into the smaller target and is resampled up, while the window keeps presenting at its
//     physical size (the swapchain is never recreated for a scale change);
//  3. recovery observations return the controller to step 0 and the next frame is direct again.
//
// The run - device creation, direct frames, downgraded frames, recovery frame, teardown - stays
// validation-clean with synchronization validation on. Skips cleanly when the platform has no
// window backend/presentation surface or the validation layer is unavailable (without it the "no
// validation error" half cannot be proven).

#include "tessera/app/window.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowDeviceState;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::rendering::rhi::Extent2D;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::scale::adaptive_extent_pixels;
using tessera::ui::style::compile_stylesheet;

// The controller's first ladder step for renderScale=1.0 and minRenderScale=0.5 (performance.md
// section 6.1: four evenly spaced steps, exact at both ends).
constexpr float kFirstDowngradeScale = 0.875f;
// Bounded pump budget: a busy GPU costs skipped iterations, never an unbounded wait.
constexpr std::uint32_t kMaxIterations = 300;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_render_scale: " << message << "\n";
    }
    return condition;
}

// A paint-data change marks paint/render dirty without touching style or layout, so each iteration
// plans a real frame instead of relying on a forced-frame loop the seam deliberately does not have.
void set_label(Node& label, std::uint32_t iteration) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "Render scale " + std::to_string(iteration);
    label.set_paint_data(std::move(data));
}

// Pumps until one more frame was presented (or the budget is spent), so every assertion below reads
// stats of a frame that really reached the swapchain.
bool pump_until_presented(WindowHost& host, std::uint32_t presented_before, Node& label,
                          std::uint32_t& iteration) {
    for (std::uint32_t attempt = 0; attempt < kMaxIterations; ++attempt) {
        set_label(label, ++iteration);
        (void)host.pump_once();
        if (host.stats().presented > presented_before) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    set_label(label, 0);

    tessera::app::WindowOptions options;
    options.title = "Tessera render scale";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.validation = true;
    // A pinned budget and ladder so the observations below decide the step deterministically
    // instead of depending on the machine's measured frame time.
    options.runtime.frame_budget_ms = 16.0;
    options.runtime.render_scale = 1.0f;
    options.runtime.min_render_scale = 0.5f;

    std::uint32_t frames = 0;
    std::uint32_t presented = 0;
    std::uint32_t scaled_frames = 0;
    std::uint32_t scaled_before_downgrade = 0;
    std::uint32_t direct_scaled_frames = 0;
    std::uint32_t scale_fallbacks = 0;
    Extent2D present_extent{};
    Extent2D direct_extent{};
    Extent2D downgraded_extent{};
    Extent2D restored_extent{};
    float direct_scale = 0.0f;
    float downgraded_scale = 0.0f;
    float restored_scale = 0.0f;
    bool direct_presented = false;
    bool downgraded_presented = false;
    bool restored_presented = false;
    bool device_ready = false;
    bool stalled = false;
    {
        WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
        if (!host.available()) {
            std::cout << "app_window_render_scale: skipped (no window backend or presentation "
                         "surface)\n";
            return 0;
        }
        if (!tessera::rendering::vulkan::validation_active()) {
            std::cout << "app_window_render_scale: skipped (validation layer unavailable)\n";
            return 0;
        }

        std::uint32_t iteration = 0;
        const WindowFrameStats& stats = host.stats();

        // 0. Startup frame: lays the scene out, so later frames are steady-state.
        (void)pump_until_presented(host, 0, label, iteration);
        // Settle the controller at step 0 deterministically: 40 met observations clear any
        // cooldown, run a full recovery (12 met frames) and then leave the counters quiet, so the
        // forced downgrade below is exactly one ladder step regardless of how long the startup
        // frame took.
        for (std::uint32_t hit = 0; hit < 40; ++hit) {
            host.host().observe_frame_time_ms(1.0);
        }

        // 1. Full quality: the frame renders directly into the swapchain image.
        direct_presented = pump_until_presented(host, stats.presented, label, iteration);
        direct_extent = stats.target_extent;
        present_extent = stats.present_extent;
        direct_scale = stats.render_scale;
        direct_scaled_frames = stats.scaled_frames;

        // 2. Force one ladder step down through the controller's observation API: four over-budget
        // frames in a row are a downgrade (performance.md section 6.1). The frame after it must use
        // the smaller target.
        for (std::uint32_t miss = 0; miss < 4; ++miss) {
            host.host().observe_frame_time_ms(50.0);
        }
        const std::uint32_t presented_before_downgrade = stats.presented;
        scaled_before_downgrade = stats.scaled_frames;
        downgraded_presented =
            pump_until_presented(host, presented_before_downgrade, label, iteration);
        downgraded_extent = stats.target_extent;
        downgraded_scale = stats.render_scale;

        // 3. Recovery: cooldown (8 observed frames) plus twelve met frames take it back to step 0.
        for (std::uint32_t hit = 0; hit < 30; ++hit) {
            host.host().observe_frame_time_ms(1.0);
        }
        const std::uint32_t presented_before_restore = stats.presented;
        restored_presented = pump_until_presented(host, presented_before_restore, label, iteration);
        restored_extent = stats.target_extent;
        restored_scale = stats.render_scale;
        frames = stats.frames;
        presented = stats.presented;
        scaled_frames = stats.scaled_frames;
        scale_fallbacks = stats.scale_fallbacks;
        device_ready = stats.device_state == WindowDeviceState::Ready;
        stalled = stats.stalled;
    }

    bool ok = true;
    ok = expect(direct_presented, "a full-quality frame presented") && ok;
    ok = expect(downgraded_presented, "a frame presented after the forced downgrade") && ok;
    ok = expect(restored_presented, "a frame presented after the recovery") && ok;
    ok = expect(device_ready, "the run reports a healthy device") && ok;
    ok = expect(!stalled, "no frame failed: the run did not stall") && ok;
    ok = expect(scale_fallbacks == 0, "the backend accepted every scaled target") && ok;

    // 1. At scale 1.0 the target is exactly the presented extent: today's direct path, no resample.
    ok = expect(direct_scale == 1.0f, "the first frame reports the default renderScale of 1.0") &&
         ok;
    ok = expect(direct_extent.width == present_extent.width &&
                    direct_extent.height == present_extent.height,
                "at scale 1.0 the render target is the presented extent") &&
         ok;
    ok = expect(present_extent.width > 0 && present_extent.height > 0,
                "the window presents at a real physical size") &&
         ok;
    ok = expect(direct_scaled_frames == 0, "no frame was resampled at scale 1.0") && ok;

    // 2. The downgraded frame renders into the documented smaller target and presents at the same
    // physical size; the swapchain is not recreated for the scale change.
    ok = expect(downgraded_scale == kFirstDowngradeScale,
                "the forced downgrade landed on the first ladder step") &&
         ok;
    ok = expect(downgraded_extent.width ==
                    adaptive_extent_pixels(present_extent.width, kFirstDowngradeScale),
                "the downgraded target width is floor(presented * renderScale)") &&
         ok;
    ok = expect(downgraded_extent.height ==
                    adaptive_extent_pixels(present_extent.height, kFirstDowngradeScale),
                "the downgraded target height is floor(presented * renderScale)") &&
         ok;
    ok = expect(downgraded_extent.width < present_extent.width &&
                    downgraded_extent.height < present_extent.height,
                "the downgraded target is smaller than the window") &&
         ok;
    ok = expect(scaled_frames > scaled_before_downgrade,
                "the downgrade presented through the resample") &&
         ok;

    // 3. Restoring returns the target to the presented extent: the direct path again.
    ok = expect(restored_scale == 1.0f, "the recovery returned the controller to step 0") && ok;
    ok = expect(restored_extent.width == present_extent.width &&
                    restored_extent.height == present_extent.height,
                "the restored target is the presented extent again") &&
         ok;
    ok = expect(presented >= 3, "at least three frames reached the swapchain") && ok;
    ok = expect(frames >= presented, "every presented frame was painted") && ok;

    // Validation-clean: teardown already ran (the host scope closed above), so this covers device
    // creation, the direct frame, the downgraded frames and the bounded drain.
    const std::uint64_t messages = tessera::rendering::vulkan::validation_message_count();
    ok = expect(messages == 0, "the run produced no validation messages") && ok;

    if (!ok) {
        std::cerr << "app_window_render_scale: failed (presented=" << presented
                  << " frames=" << frames << " target=" << downgraded_extent.width << "x"
                  << downgraded_extent.height << " present=" << present_extent.width << "x"
                  << present_extent.height << " scaled_frames=" << scaled_frames
                  << " validation=" << messages << ")\n";
        return 1;
    }
    std::cout << "app_window_render_scale: ok (presented=" << presented
              << " direct=" << direct_extent.width << "x" << direct_extent.height
              << " downgraded=" << downgraded_extent.width << "x" << downgraded_extent.height
              << " restored=" << restored_extent.width << "x" << restored_extent.height
              << " scaled_frames=" << scaled_frames << " validation=" << messages << ")\n";
    return 0;
}
