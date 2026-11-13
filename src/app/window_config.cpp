// SPDX-License-Identifier: MIT

#include "window_config.hpp"

#include <cmath>

namespace tessera::app {

ui::frame::FramePipelineConfig window_pipeline_config(const RuntimeConfig& runtime) {
    ui::frame::FramePipelineConfig config;
    config.interpolation = runtime.animation_interpolation;
    config.interpolation_epsilon_px = runtime.interpolation_epsilon_px;
    return config;
}

WindowPausePolicy window_pause_policy(const RuntimeConfig& runtime) {
    WindowPausePolicy policy;
    // The one key describes an invisible window, not one particular lifecycle event: both the
    // occluded and minimized sources follow it (section 5.8).
    policy.pause_when_occluded = runtime.pause_when_occluded;
    policy.pause_when_minimized = runtime.pause_when_occluded;
    return policy;
}

AdaptiveQualityPolicy window_adaptive_quality_policy(const RuntimeConfig& runtime,
                                                     double refresh_hz) {
    AdaptiveQualityPolicy policy;
    policy.adaptive_quality = runtime.adaptive_quality;
    policy.power_policy = runtime.power_policy;
    policy.render_scale = runtime.render_scale;
    policy.min_render_scale = runtime.min_render_scale;
    policy.frame_budget_ms = runtime.frame_budget_ms;
    // The budget derives from the effective present rate (section 5.2); with neither a cap nor a
    // requested refresh the framework targets its default 60 Hz tick baseline.
    if (runtime.max_fps != 0) {
        policy.target_fps = static_cast<double>(runtime.max_fps);
    } else if (runtime.preferred_refresh_rate != 0) {
        policy.target_fps = static_cast<double>(runtime.preferred_refresh_rate);
    } else {
        policy.target_fps = 60.0;
    }
    // The step-3 present cap: `maxFps` is the configured cap and the hard ceiling, and the display
    // refresh is the divisor base. A non-finite or negative report is "unknown", never a rate.
    policy.max_fps = static_cast<double>(runtime.max_fps);
    policy.refresh_hz = std::isfinite(refresh_hz) && refresh_hz > 0.0 ? refresh_hz : 0.0;
    return policy;
}

} // namespace tessera::app
