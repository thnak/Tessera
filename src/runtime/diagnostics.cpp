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

void FrameDiagnostics::reset() {
    last_ = FrameDiagnosticsSample{};
    frames_ = 0;
    total_steps_ = 0;
    total_completions_ = 0;
    total_damage_rects_ = 0;
    total_scissors_ = 0;
    total_animation_samples_ = 0;
    full_redraw_frames_ = 0;
}

} // namespace tessera::runtime
