// SPDX-License-Identifier: MIT
//
// The adaptive-quality stage of AppHost::Impl (god-type audit slice 10, section 3.4).
//
// The host used to run the whole adaptive-quality ladder inline in host.cpp: pick the frame-time
// signal, record it, build the two-effect stage probe, observe the controller, record the decision,
// then apply it to the pipeline and the animation runtime. That is a policy with four consumers,
// and none of it is about the scene, the stylesheet or the window - so it is a type here.
//
// The split matters for a second reason: the *application* of a decision has ordering rules that
// were only true because they sat in one function. A reduced-effects flip must request a full
// repaint (a settled on-demand scene has nothing else to repaint it), while a paused-animation flip
// must mark the loop dirty (resuming has to wake an idle loop). Both are now pinned by
// tests/unit/app_quality_stage_tests.cpp instead of being inferred from the surrounding code.

#pragma once

#include "adaptive_quality.hpp"
#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <optional>

namespace tessera::app::detail {

// What the stage can reach on the host. Function pointers plus one context, rather than a pointer
// to AppHost::Impl, so the stage is testable without constructing a host - the same shape
// AdaptiveQualityStageProbe already uses.
struct QualityStageContext {
    // True when some visible node paints a blurred outer shadow: the effect the first ladder
    // position reduces. Read from the outsets the style sync already stores, so no style
    // resolution.
    bool (*scene_has_blurred_shadow)(const void* context) noexcept = nullptr;
    // True while any non-essential animation is running.
    bool (*non_essential_animations)(const void* context) noexcept = nullptr;

    // Applies a reduced-effects flip: set the pipeline flag and request a full repaint.
    void (*set_reduce_effects)(void* context, bool reduced) = nullptr;
    // Applies a suspended-animations flip, and marks the loop dirty when animations resume.
    void (*set_animations_paused)(void* context, bool paused) = nullptr;

    // The counters the stage records into. Borrowed; outlives the stage.
    runtime::FrameDiagnostics* diagnostics = nullptr;
    void* context = nullptr;
};

// One host's adaptive-quality ladder: the controller plus the recording and application rules.
class QualityStage {
public:
    QualityStage(QualityStageContext context, runtime::AdaptiveQualityConfig config);

    // One observed frame. `gpu_frame_time_ms` is present only when the device probe produced a
    // usable interval; the pure selection rule decides which number the controller sees. Returns
    // the decision in force after the observation.
    const runtime::AdaptiveQualitySample& observe(double cpu_frame_time_ms,
                                                  std::optional<double> gpu_frame_time_ms);

    // Records the decision in force without observing a frame, so the host reports the configured
    // scale before its first frame instead of a default 1.0.
    void publish();

    // The current decision, for a freshly created pipeline to adopt (AppHost::resize recreates it).
    const runtime::AdaptiveQualitySample& decision() const {
        return controller_.decision();
    }

    [[nodiscard]] runtime::AdaptiveQualityController& controller() {
        return controller_;
    }
    [[nodiscard]] const runtime::AdaptiveQualityController& controller() const {
        return controller_;
    }

private:
    QualityStageContext context_;
    runtime::AdaptiveQualityController controller_;
};

} // namespace tessera::app::detail
