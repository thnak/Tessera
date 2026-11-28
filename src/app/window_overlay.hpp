// SPDX-License-Identifier: MIT
//
// The window driver's diagnostics overlay model (issue #1024, runtime config key
// `diagnosticsOverlay`): the frame-time meter that turns the observed frame cost into the fps and
// frame-ms the overlay prints, and the one function that folds the frame, the budget and the layer
// cache into an `OverlayFrame`. Header-only and pure, so the numbers are tested without a window or
// a device; the draw itself (window_overlay.cpp) only places the lines.

#pragma once

#include "layer_overlay.hpp"
#include "tessera/diagnostics/overlay.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::app {

// Exponential moving average of the frame cost. The first sample seeds it, so the readout is exact
// for a steady frame time and settles in about ten frames after a change. No clock is read here:
// the driver passes the cost it already measured for the adaptive controller.
class FrameRateMeter {
public:
    // `budget_ms` of 0 is no budget; a sample above a configured budget is an overrun.
    void record(double frame_ms, double budget_ms = 0.0) noexcept {
        if (!(frame_ms > 0.0)) {
            return; // a zero or negative cost would print an infinite fps
        }
        if (budget_ms > 0.0 && frame_ms > budget_ms) {
            ++overruns_;
        }
        average_ms_ = samples_ == 0 ? frame_ms : average_ms_ + (frame_ms - average_ms_) * kWeight;
        ++samples_;
    }
    [[nodiscard]] double frame_ms() const noexcept {
        return average_ms_;
    }
    [[nodiscard]] std::uint64_t overruns() const noexcept {
        return overruns_;
    }
    // 0 until a sample arrives, which the overlay model omits rather than print a made-up rate.
    [[nodiscard]] double fps() const noexcept {
        return average_ms_ > 0.0 ? 1000.0 / average_ms_ : 0.0;
    }

private:
    static constexpr double kWeight = 0.1;
    double average_ms_ = 0.0;
    std::uint64_t samples_ = 0;
    std::uint64_t overruns_ = 0;
};

struct WindowOverlayInput {
    std::uint64_t frame = 0;
    std::size_t scissors = 0;
    bool full_redraw = false;
    double budget_ms = 0.0;
    bool layer_cache_enabled = false;
};

[[nodiscard]] inline diagnostics::OverlayFrame
build_window_overlay_frame(const WindowOverlayInput& input, const FrameRateMeter& meter,
                           const rendering::gpu::RetainedLayerStats& cache,
                           const ui::paint::LayerPromotionStats& promotion) noexcept {
    diagnostics::OverlayFrame frame;
    frame.frame = input.frame;
    frame.steps = 1;
    frame.damage_rects = input.scissors;
    frame.scissors = input.scissors;
    frame.full_redraw = input.full_redraw;
    frame.fps = meter.fps();
    frame.frame_ms = meter.frame_ms();
    frame.budget_ms = input.budget_ms;
    frame.budget_overruns = meter.overruns();
    fill_layer_overlay(frame, input.layer_cache_enabled, cache, promotion);
    return frame;
}

} // namespace tessera::app
