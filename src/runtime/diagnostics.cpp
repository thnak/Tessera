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
    if (sample.full_redraw) {
        ++full_redraw_frames_;
    }
}

void FrameDiagnostics::record_adaptive_quality(const AdaptiveQualitySample& sample) noexcept {
    // The cap is part of the sample, so a change is visible even when only the cap moved (a step
    // whose scale was already at the floor). Comparing against the previous sample keeps the
    // counter honest without the caller having to report the cap separately.
    const bool cap_changed = sample.present_cap_hz != adaptive_quality_.present_cap_hz;
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
}

void FrameDiagnostics::reset() {
    last_ = FrameDiagnosticsSample{};
    adaptive_quality_ = AdaptiveQualitySample{};
    frames_ = 0;
    total_steps_ = 0;
    total_completions_ = 0;
    total_damage_rects_ = 0;
    total_scissors_ = 0;
    total_animation_samples_ = 0;
    full_redraw_frames_ = 0;
    adaptive_quality_changes_ = 0;
    adaptive_quality_downgrades_ = 0;
    adaptive_present_cap_changes_ = 0;
}

} // namespace tessera::runtime
