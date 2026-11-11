// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

namespace tessera::runtime {
class RuntimeLoop;
}

namespace tessera::ui::frame {

struct FramePipelineConfig {
    float dpi_scale = 1.0f;
    float damage_margin = 0.0f;
    rendering::RedrawPolicy redraw_policy{};
    // Paint-only interpolation between ticks (runtime-config `animationInterpolation`). On by
    // default, like the runtime config; a render that coincides with a tick (alpha 0) paints the
    // previous sample, and renders between ticks blend toward the current one.
    bool interpolation = true;
};

struct FrameTick {
    animation::AnimationTickResult animation;
    bool needs_style = false;
    bool needs_layout = false;
};

struct FrameRender {
    layout::DamagePassResult damage;
    rendering::RedrawPlan redraw_plan;
    bool full_redraw = false;
    bool should_render = false;
};

struct FramePaint {
    paint::PaintStats stats;
    bool painted = false;
};

class FramePipeline {
public:
    FramePipeline(animation::AnimationRuntime& animations, runtime::RuntimeLoop& loop,
                  layout::DamageTracker& damage_tracker, rendering::DamageAccumulator& damage,
                  DirtyPass& dirty, FramePipelineConfig config = {});

    // Two-argument tick: advances the simulation and paints the authoritative state (no
    // interpolation). Use the alpha overload below when a render falls between ticks.
    FrameTick tick(Node& root, double dt);

    // Alpha-aware tick. `alpha` is `runtime::interpolation_alpha` for the render about to happen:
    // the fraction of a fixed step elapsed since this tick. Passing it is what enables
    // interpolation for this frame, so the two-argument overload above never changes what a caller
    // painted before this seam existed. Alpha only affects paint; simulation, dirty domains and
    // damage are identical for any alpha, so a scene that is clean before the call stays clean and
    // still idles. Out-of-range or non-finite values clamp to 0.
    FrameTick tick(Node& root, double dt, double alpha);

    // Copies z-index, transform and box-shadow reach from computed style onto the scene (see
    // paint::sync_scene_style). Call it after the caller's style/layout work and before
    // finish_frame(), so the paint/render dirtiness it raises reaches damage. It only runs when
    // `tick` reported style or layout work (or `force` is set); a steady frame costs nothing and
    // returns an all-zero result.
    paint::SceneStyleSyncResult sync_style(Node& root, paint::StyleProvider& styles,
                                           const FrameTick& tick, bool force = false) const;

    FrameRender finish_frame(Node& root);

    // Paints the whole scene into `sink` only when the most recent finish_frame() decided the frame
    // is due (`FrameRender::should_render`), or when `force` is set. tick() clears the decision, so
    // paint after finish_frame(). The paint is full-frame: the redraw plan's damage scissors are
    // not applied yet (damage-clipped painting is a later slice). The scene is read-only here:
    // dirty flags, damage, and redraw planning are untouched, and painting twice in one frame is
    // allowed and deterministic.
    FramePaint paint_frame(const Node& root, paint::StyleProvider& styles, paint::PaintSink& sink,
                           bool force = false) const;

    // Damage-clipped variant: paints only the parts of the scene inside `plan`'s scissor regions.
    // A full-redraw plan forwards to the unclipped sink, so its commands are byte-identical to the
    // full paint above (its scissor list is ignored). An empty partial plan paints nothing, even
    // with `force`. A partial plan clips every command per region, in region order: a command fully
    // inside a region is unchanged, a partially covered one is trimmed to the intersection, and one
    // outside every region is dropped, so paint order is preserved. `FramePaint::stats` are the
    // pre-clip paint-pass counts. `force` still overrides the frame-due check for a non-empty plan.
    FramePaint paint_frame(const Node& root, paint::StyleProvider& styles, paint::PaintSink& sink,
                           const rendering::RedrawPlan& plan, bool force = false) const;

    // True when the most recent finish_frame() reported should_render and tick() has not run since.
    [[nodiscard]] bool frame_due() const {
        return frame_due_;
    }

    // The paint-only view for the samples of the last two ticks at the last alpha. Empty when
    // interpolation is disabled, when the last tick sampled no paint-visible property, or when the
    // render coincides with the first sample (nothing older to blend from). paint_frame() applies
    // it automatically; expose it for callers that paint through `paint::paint_scene` directly.
    [[nodiscard]] InterpolationPaintView interpolation_view() const;

    // Installs the per-window update scheduler (issue #5). Its wake hook is bound to the loop's
    // `request_frame()`, and every `tick()` flushes it first, so state set by setters, input
    // handlers or async completions is applied before animation, style and layout of that tick.
    // Pass nullptr to uninstall. The scheduler must outlive the pipeline or be uninstalled first.
    void set_update_scheduler(UpdateScheduler* scheduler);
    [[nodiscard]] UpdateScheduler* update_scheduler() const {
        return scheduler_;
    }

    void set_interpolation(bool enabled) {
        config_.interpolation = enabled;
        if (!enabled) {
            previous_samples_.clear();
            current_samples_.clear();
        }
    }
    [[nodiscard]] bool interpolation() const {
        return config_.interpolation;
    }
    // The clamped alpha used by the most recent tick().
    [[nodiscard]] double interpolation_alpha() const {
        return alpha_;
    }

private:
    FrameTick tick_impl(Node& root, double dt, double alpha, bool interpolate);

    animation::AnimationRuntime* animations_ = nullptr;
    runtime::RuntimeLoop* loop_ = nullptr;
    layout::DamageTracker* damage_tracker_ = nullptr;
    rendering::DamageAccumulator* damage_ = nullptr;
    DirtyPass* dirty_ = nullptr;
    UpdateScheduler* scheduler_ = nullptr;
    FramePipelineConfig config_;
    bool frame_due_ = false;
    PaintSamples previous_samples_;
    PaintSamples current_samples_;
    double alpha_ = 0.0;
    bool interpolate_ = false;
};

} // namespace tessera::ui::frame
