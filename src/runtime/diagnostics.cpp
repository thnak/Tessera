// SPDX-License-Identifier: MIT

#include "tessera/runtime/diagnostics.hpp"

namespace tessera::runtime {

void FrameDiagnostics::record(FrameDiagnosticsSample sample) {
    ++frames_;
    sample.frame_index = frames_;
    last_ = sample;

    total_steps_ += sample.steps;
    total_completions_ += sample.completions;
    total_damage_rects_ += sample.damage_rects;
    total_scissors_ += sample.redraw_scissors;
    total_animation_samples_ += sample.animation_samples;
    total_glass_ms_ += sample.glass_ms;
    total_glass_backdrops_ += sample.glass_backdrop_count;
    if (sample.glass_ms > 0.0 || sample.glass_backdrop_count > 0) {
        ++glass_frames_;
    }
    if (sample.full_redraw) {
        ++full_redraw_frames_;
    }
}

void FrameDiagnostics::record_adaptive_quality(const AdaptiveQualitySample& sample) noexcept {
    // The cap is part of the sample, so a change is visible even when only the cap moved (a step
    // whose scale was already at the floor). Comparing against the previous sample keeps the
    // counter honest without the caller having to report the cap separately.
    const bool cap_changed = sample.present_cap_hz != adaptive_quality_.present_cap_hz;
    const bool effects_changed = sample.effects_reduced != adaptive_quality_.effects_reduced;
    const bool pause_changed = sample.animations_paused != adaptive_quality_.animations_paused;
    adaptive_quality_ = sample;
    if (!sample.changed) {
        return;
    }
    ++adaptive_quality_changes_;
    if (sample.downgrade) {
        ++adaptive_quality_downgrades_;
    }
    if (cap_changed) {
        ++adaptive_present_cap_changes_;
    }
    if (effects_changed) {
        ++adaptive_effect_changes_;
    }
    if (pause_changed) {
        ++adaptive_animation_pause_changes_;
    }
}

void FrameDiagnostics::record_frame_time_source(FrameTimeSource source) noexcept {
    frame_time_source_ = source;
    if (source == FrameTimeSource::Gpu) {
        ++gpu_frame_time_samples_;
    } else {
        ++cpu_frame_time_samples_;
    }
}

void FrameDiagnostics::reset() {
    last_ = FrameDiagnosticsSample{};
    adaptive_quality_ = AdaptiveQualitySample{};
    frame_time_source_ = FrameTimeSource::Cpu;
    gpu_frame_time_samples_ = 0;
    cpu_frame_time_samples_ = 0;
    frames_ = 0;
    total_steps_ = 0;
    total_completions_ = 0;
    total_damage_rects_ = 0;
    total_scissors_ = 0;
    total_animation_samples_ = 0;
    full_redraw_frames_ = 0;
    total_glass_ms_ = 0.0;
    total_glass_backdrops_ = 0;
    glass_frames_ = 0;
    adaptive_quality_changes_ = 0;
    adaptive_quality_downgrades_ = 0;
    adaptive_present_cap_changes_ = 0;
    adaptive_effect_changes_ = 0;
    adaptive_animation_pause_changes_ = 0;
}

} // namespace tessera::runtime
