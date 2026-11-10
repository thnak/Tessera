// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"

namespace tessera::runtime {
class RuntimeLoop;
}

namespace tessera::ui::frame {

struct FramePipelineConfig {
    float dpi_scale = 1.0f;
    float damage_margin = 0.0f;
    rendering::RedrawPolicy redraw_policy{};
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

    FrameTick tick(Node& root, double dt);

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

private:
    animation::AnimationRuntime* animations_ = nullptr;
    runtime::RuntimeLoop* loop_ = nullptr;
    layout::DamageTracker* damage_tracker_ = nullptr;
    rendering::DamageAccumulator* damage_ = nullptr;
    DirtyPass* dirty_ = nullptr;
    FramePipelineConfig config_;
    bool frame_due_ = false;
};

} // namespace tessera::ui::frame
