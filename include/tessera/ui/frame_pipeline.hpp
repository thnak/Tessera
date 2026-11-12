// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/spring_runtime.hpp"
#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <unordered_map>

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
    // Per-node paint cache (docs/design/dirty-rendering.md section 5.4). On by default: the
    // pipeline owns one `paint::PaintCache` and passes it to both paint_frame overloads, so a
    // clean node's recorded sink calls are replayed instead of re-emitted. When false the context
    // gets a null cache and the paint pass is byte-identical to the pre-cache pipeline. Cache
    // limits stay at `PaintCacheConfig`'s defaults; this switch only enables or bypasses it.
    bool paint_cache = true;
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

    // --- Per-node paint cache (docs/design/dirty-rendering.md section 5.4) ----------------------
    // The pipeline owns the cache (`FramePipelineConfig::paint_cache`) and passes it into both
    // paint_frame overloads; painting is its only reader and writer. The cache is keyed by node id
    // and stores one node's own sink calls, so one pipeline must paint one scene tree.
    //
    // Invalidation points. A record is replayed only while every input it was recorded under is
    // unchanged (see paint_cache.hpp), and a replay is byte-identical to re-emitting the node:
    //   - a node's own dirty flags are part of the lookup: a style/property change that marks the
    //     node re-records it, so no call is needed here for a normal property edit;
    //   - invalidate_paint_cache(): required whenever a node's computed style can change while its
    //     own dirty flags stay Clean - a rebuilt selector/style tree, a new stylesheet, theme or
    //     root-style generation, a swapped StyleProvider, or a restyle whose effect is inherited
    //     into clean descendants. AppHost calls it from begin_frame() on every style-tree rebuild
    //     (see HostFrame::style_rebuilt); a caller that swaps a style generation and cannot prove
    //     every affected node was marked must call it too;
    //   - a `PaintView` (interpolation) bypasses the cache instead of invalidating it: such a paint
    //     walks the uncached path and leaves the records alone, so they stay valid for the next
    //     non-interpolated paint of the same generation;
    //   - re-creating the pipeline (AppHost::resize does) drops the cache with it; a cold cache is
    //     always safe. Over-invalidating costs hit rate only, under-invalidating replays stale
    //     output, so any doubt resolves to an invalidate call.
    void invalidate_paint_cache();

    // Cumulative counters of the pipeline-owned cache. After a paint that re-recorded nothing,
    // `hits` equals the visible node count and the `recorded_draws` delta is zero. The returned
    // reference is owned by the pipeline and refreshed by every call; read it after paint_frame().
    [[nodiscard]] const paint::PaintCacheStats& paint_cache_stats() const;

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

    // The pipeline-owned spring runtime for style-property springs (start/retarget through it).
    // Each tick advances it by the same dt as the CSS animation runtime, merges its samples into
    // `FrameTick::animation` (so dirty marking and paint interpolation treat them like any other
    // animation sample), requests a frame while a spring runs, drops springs whose node left the
    // tree, and follows the animation runtime's reduced-motion flag (springs snap to target).
    [[nodiscard]] animation::SpringRuntime& springs() {
        return springs_;
    }
    [[nodiscard]] const animation::SpringRuntime& springs() const {
        return springs_;
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
    void tick_springs(Node& root, double dt, animation::AnimationTickResult& into,
                      std::unordered_map<NodeId, Node*>& index);

    animation::AnimationRuntime* animations_ = nullptr;
    runtime::RuntimeLoop* loop_ = nullptr;
    layout::DamageTracker* damage_tracker_ = nullptr;
    rendering::DamageAccumulator* damage_ = nullptr;
    DirtyPass* dirty_ = nullptr;
    UpdateScheduler* scheduler_ = nullptr;
    animation::SpringRuntime springs_;
    FramePipelineConfig config_;
    bool frame_due_ = false;
    // Paint records for the tree this pipeline paints; mutable because painting is const.
    mutable paint::PaintCache paint_cache_;
    mutable paint::PaintCacheStats paint_cache_stats_snapshot_;
    PaintSamples previous_samples_;
    PaintSamples current_samples_;
    double alpha_ = 0.0;
    bool interpolate_ = false;
};

} // namespace tessera::ui::frame
