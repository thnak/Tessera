// SPDX-License-Identifier: MIT
//
// The adaptive present cap on the real window path (docs/design/performance.md section 6, step 3;
// docs/specs/runtime-config-v1.md sections 5.2 and 5.7). A hidden window plus a Vulkan swapchain is
// driven through the deterministic single-iteration seam WindowHost::pump_once():
//
//  1. at full quality the pacer applies the configured cap: on a 60 Hz display with no `maxFps` the
//     present interval is one refresh (1/60 s);
//  2. four over-budget observations move the controller one ladder step down; the decided cap is
//     refresh/2 = 30 Hz, so the pacer's interval doubles to 2/60 s - applied through the pacer's
//     existing rate-change path, with no blocking wait and no swapchain recreation;
//  3. recovery observations return the controller to step 0 and the interval returns to 1/60 s.
//
// The run stays validation-clean. Skips cleanly when the platform has no window
// backend/presentation surface or the validation layer is unavailable (without it the "no
// validation error" half cannot be proven).

#include "tessera/app/window.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowDeviceState;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

// The display refresh the test pins, and the cap the first ladder step derives from it.
constexpr double kRefreshHz = 60.0;
constexpr double kFirstDowngradeCapHz = 30.0;
// Bounded pump budget: a busy GPU costs skipped iterations, never an unbounded wait.
constexpr std::uint32_t kMaxIterations = 300;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_present_cap: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

// A paint-data change marks paint/render dirty without touching style or layout, so each iteration
// plans a real frame instead of relying on a forced-frame loop the seam deliberately does not have.
void set_label(Node& label, std::uint32_t iteration) {
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "Present cap " + std::to_string(iteration);
    label.set_paint_data(std::move(data));
}

// Pumps until one more frame was presented (or the budget is spent), so every assertion below reads
// stats of a frame that really reached the swapchain. Unlike the render-scale test this one runs
// with a real present cadence (the cap is the point), so the loop parks briefly between iterations:
// the driver's own run() parks on the pacer deadline, and pump_once() deliberately does not.
bool pump_until_presented(WindowHost& host, std::uint32_t presented_before, Node& label,
                          std::uint32_t& iteration) {
    for (std::uint32_t attempt = 0; attempt < kMaxIterations; ++attempt) {
        set_label(label, ++iteration);
        (void)host.pump_once();
        if (host.stats().presented > presented_before) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    options.title = "Tessera present cap";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;
    options.validation = true;
    // The step-3 inputs: a pinned display refresh and no configured cap, so the divisor base is the
    // refresh itself and the documented sequence is 30, 20, 15, 12 Hz.
    options.refresh_hz = kRefreshHz;
    options.max_fps = 0.0;
    // A pinned budget and ladder so the observations below decide the step deterministically
    // instead of depending on the machine's measured frame time.
    options.runtime.frame_budget_ms = 16.0;
    options.runtime.render_scale = 1.0f;
    options.runtime.min_render_scale = 0.5f;

    std::uint32_t frames = 0;
    std::uint32_t presented = 0;
    std::uint32_t cap_changes = 0;
    double direct_cap = 0.0;
    double direct_interval = 0.0;
    double downgraded_cap = 0.0;
    double downgraded_interval = 0.0;
    double restored_cap = 0.0;
    double restored_interval = 0.0;
    bool direct_presented = false;
    bool downgraded_presented = false;
    bool restored_presented = false;
    bool device_ready = false;
    bool stalled = false;
    {
        WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
        if (!host.available()) {
            std::cout << "app_window_present_cap: skipped (no window backend or presentation "
                         "surface)\n";
            return 0;
        }
        if (!tessera::rendering::vulkan::validation_active()) {
            std::cout << "app_window_present_cap: skipped (validation layer unavailable)\n";
            return 0;
        }

        std::uint32_t iteration = 0;
        const WindowFrameStats& stats = host.stats();

        // 0. Startup frame: lays the scene out, so later frames are steady-state.
        (void)pump_until_presented(host, 0, label, iteration);
        // Settle the controller at step 0 deterministically: 40 met observations clear any cooldown
        // and run a full recovery, so the forced downgrade below is exactly one ladder step.
        for (std::uint32_t hit = 0; hit < 40; ++hit) {
            host.host().observe_frame_time_ms(1.0);
        }

        // 1. Full quality: the pacer applies the configured cap (0 = uncapped, so the 60 Hz display
        // cadence is the interval).
        direct_presented = pump_until_presented(host, stats.presented, label, iteration);
        direct_cap = stats.present_cap_hz;
        direct_interval = stats.present_interval_seconds;

        // 2. Force one ladder step down: four over-budget frames in a row are a downgrade
        // (performance.md section 6.1). The decided cap is refresh/2, so the next frame's present
        // interval doubles.
        // The ladder's first position only reduces effects (ADR-019 section 8): spend it with the
        // four misses, wait out the eight-frame cooldown (those frames are not evidence), then four
        // more misses reach the first scale step, which is the one this test observes.
        for (std::uint32_t miss = 0; miss < 16; ++miss) {
            host.host().observe_frame_time_ms(50.0);
        }
        const std::uint32_t presented_before_downgrade = stats.presented;
        downgraded_presented =
            pump_until_presented(host, presented_before_downgrade, label, iteration);
        downgraded_cap = stats.present_cap_hz;
        downgraded_interval = stats.present_interval_seconds;

        // 3. Recovery: cooldown (8 observed frames) plus twelve met frames take it back to step 0.
        for (std::uint32_t hit = 0; hit < 30; ++hit) {
            host.host().observe_frame_time_ms(1.0);
        }
        const std::uint32_t presented_before_restore = stats.presented;
        restored_presented = pump_until_presented(host, presented_before_restore, label, iteration);
        restored_cap = stats.present_cap_hz;
        restored_interval = stats.present_interval_seconds;
        frames = stats.frames;
        presented = stats.presented;
        cap_changes = stats.present_cap_changes;
        device_ready = stats.device_state == WindowDeviceState::Ready;
        stalled = stats.stalled;
    }

    bool ok = true;
    ok = expect(direct_presented, "a full-quality frame presented") && ok;
    ok = expect(downgraded_presented, "a frame presented after the forced downgrade") && ok;
    ok = expect(restored_presented, "a frame presented after the recovery") && ok;
    ok = expect(device_ready, "the run reports a healthy device") && ok;
    ok = expect(!stalled, "no frame failed: the run did not stall") && ok;

    // 1. Full quality: the configured cap (uncapped) and the display cadence.
    ok = expect(direct_cap == 0.0, "full quality keeps the configured cap (uncapped)") && ok;
    ok = expect(near(direct_interval, 1.0 / kRefreshHz),
                "the full-quality interval is one display refresh") &&
         ok;

    // 2. The downgrade moved the cap to refresh/2 and the pacer's interval to two refreshes.
    ok = expect(near(downgraded_cap, kFirstDowngradeCapHz),
                "the downgrade lowered the cap to the next refresh divisor") &&
         ok;
    ok = expect(near(downgraded_interval, 2.0 / kRefreshHz),
                "the pacer applies the cap: the present interval doubled") &&
         ok;
    ok = expect(cap_changes >= 1, "the cap change is counted") && ok;

    // 3. The recovery restored the configured cap and the display cadence.
    ok = expect(restored_cap == 0.0, "the recovery restored the configured cap") && ok;
    ok = expect(near(restored_interval, 1.0 / kRefreshHz),
                "the restored interval is one display refresh again") &&
         ok;
    ok = expect(presented >= 3, "at least three frames reached the swapchain") && ok;
    ok = expect(frames >= presented, "every presented frame was painted") && ok;

    // Validation-clean: teardown already ran (the host scope closed above).
    const std::uint64_t messages = tessera::rendering::vulkan::validation_message_count();
    ok = expect(messages == 0, "the run produced no validation messages") && ok;

    if (!ok) {
        std::cerr << "app_window_present_cap: failed (presented=" << presented
                  << " frames=" << frames << " direct_cap=" << direct_cap
                  << " downgraded_cap=" << downgraded_cap << " restored_cap=" << restored_cap
                  << " downgraded_interval=" << downgraded_interval
                  << " cap_changes=" << cap_changes << " validation=" << messages << ")\n";
        return 1;
    }
    std::cout << "app_window_present_cap: ok (presented=" << presented
              << " direct_cap=" << direct_cap << " downgraded_cap=" << downgraded_cap
              << " restored_cap=" << restored_cap << " downgraded_interval=" << downgraded_interval
              << " cap_changes=" << cap_changes << " validation=" << messages << ")\n";
    return 0;
}
