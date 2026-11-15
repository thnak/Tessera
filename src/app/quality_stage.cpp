// SPDX-License-Identifier: MIT

#include "quality_stage.hpp"

namespace tessera::app::detail {

QualityStage::QualityStage(QualityStageContext context, runtime::AdaptiveQualityConfig config)
    : context_(context),
      controller_(config) {}

void QualityStage::publish() {
    if (context_.diagnostics != nullptr) {
        context_.diagnostics->record_adaptive_quality(controller_.decision());
    }
}

const runtime::AdaptiveQualitySample&
QualityStage::observe(double cpu_frame_time_ms, std::optional<double> gpu_frame_time_ms) {
    // The pure rule (frame_time_source.hpp) picks the signal; the capability decision was already
    // made by the device probe, so the worst case here is a CPU fallback, never a guessed GPU
    // number. Recording the source always keeps the latest value readable.
    const runtime::FrameTimeSample sample =
        runtime::select_frame_time(cpu_frame_time_ms, gpu_frame_time_ms);
    if (context_.diagnostics != nullptr) {
        context_.diagnostics->record_frame_time_source(sample.source);
    }

    // The probe is read only when the controller is about to change position
    // (adaptive_quality.hpp), so the scene walk costs nothing on an ordinary frame.
    const runtime::AdaptiveQualityStageProbe probe{
        context_.scene_has_blurred_shadow, context_.non_essential_animations, context_.context};
    // `observe` returns a reference into the controller, which outlives every caller here.
    static_cast<void>(controller_.observe(sample.frame_time_ms, probe));
    const runtime::AdaptiveQualitySample& decision = controller_.decision();
    if (context_.diagnostics != nullptr) {
        context_.diagnostics->record_adaptive_quality(decision);
    }

    // Applying a flip is the host's job, but *whether* one happened is this stage's answer, and the
    // two flips have different wake requirements: reduced effects changes pixels (a full repaint),
    // paused animations must wake an idle loop on resume.
    if (context_.set_reduce_effects != nullptr) {
        context_.set_reduce_effects(context_.context, decision.effects_reduced);
    }
    if (context_.set_animations_paused != nullptr) {
        context_.set_animations_paused(context_.context, decision.animations_paused);
    }
    return decision;
}

} // namespace tessera::app::detail
