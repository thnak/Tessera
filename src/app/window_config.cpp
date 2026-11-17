// SPDX-License-Identifier: MIT

#include "window_config.hpp"

#include <cmath>

namespace tessera::app {

ui::frame::FramePipelineConfig window_pipeline_config(const RuntimeConfig& runtime) {
    ui::frame::FramePipelineConfig config;
    config.interpolation = runtime.animation_interpolation;
    config.interpolation_epsilon_px = runtime.interpolation_epsilon_px;
    // Glass cost limits (glass-v1 sections 5 and 8): the same pair bounds the emitted sigma, the
    // half-resolution decision and the 3 sigma damage dependency, so one config drives all three.
    config.glass.max_blur_radius = runtime.glass_max_blur_radius;
    config.glass.max_backdrop_area = runtime.glass_max_backdrop_area;
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

ui::scale::AaFacts window_aa_facts(const hal::WindowMetrics& metrics) noexcept {
    ui::scale::AaFacts facts;
    facts.render_scale = metrics.dpi_scale;
    facts.pixel_geometry = metrics.pixel_geometry;
    facts.transparent_window = metrics.transparent;
    facts.orientation = metrics.orientation;
    return facts;
}

ui::scale::AaPolicy window_aa_policy(const hal::WindowMetrics& metrics,
                                     const WindowAaOptions& renderer) noexcept {
    ui::scale::AaConfig config;
    // No `renderer.textAa` setting exists on the window surface yet, so `Auto` is the whole input;
    // it resolves to grayscale until the owner decision (display-aware-rendering.md section 8).
    config.text_aa = ui::scale::TextAaSetting::Auto;
    config.aa_enabled = renderer.aa_enabled;
    config.msaa_samples = renderer.msaa_samples;
    config.reduced_quality = renderer.reduced_quality;
    // The switch, the renderer's golden flag and a renderer with AA disabled all mean the same
    // thing to text: pin the policy. An AA-off renderer still resolves text hinting from the facts
    // otherwise, which would make the switch's behavior depend on a shader setting.
    config.golden_mode = !renderer.display_aware_aa || !renderer.aa_enabled || renderer.golden_mode;
    return ui::scale::resolve_aa_policy(window_aa_facts(metrics), config);
}

} // namespace tessera::app
