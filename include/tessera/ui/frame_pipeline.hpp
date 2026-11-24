// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/spring_runtime.hpp"
#include "tessera/ui/animation/style_bridge.hpp"
#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
    // Duplicate-present suppression for interpolated frames (game-loop.md section 3.2, #148). An
    // interpolated transform adds its painted box to damage only when a corner moved at least this
    // many *device* pixels since the last frame that damaged it; below that the image is the one
    // already on screen. It is a pixel distance, not an alpha fraction, because the same alpha is a
    // different distance at different tick rates. 0 disables suppression. It never wakes the loop.
    float interpolation_epsilon_px = 0.0625f;
    // Per-node paint cache (docs/design/dirty-rendering.md section 5.4). On by default: the
    // pipeline owns one `paint::PaintCache` and passes it to both paint_frame overloads, so a
    // clean node's recorded sink calls are replayed instead of re-emitted. When false the context
    // gets a null cache and the paint pass is byte-identical to the pre-cache pipeline. Cache
    // limits stay at `PaintCacheConfig`'s defaults; this switch only enables or bypasses it.
    bool paint_cache = true;
    // Glass cost limits (docs/specs/glass-v1.md sections 5 and 8), from the runtime config keys
    // `glassMaxBlurRadius` and `glassMaxBackdropArea`. They are carried into both the style sync
    // (the 3 sigma damage dependency) and the paint context (the emitted sigma / half-resolution
    // decision), so one pair of values bounds the effect everywhere.
    rendering::gpu::GlassLimits glass{};
    // Paint-pass culling (issue #487, docs/design/dirty-rendering.md section 6.8), from the runtime
    // config key `cullDraws`: the same "CPU culling" concept as the renderer's quad cull, so one
    // key turns both stages off for a full-work comparison. On by default. When the redraw plan is
    // partial, the damage-clipped `paint_frame` overload hands the paint pass the bounding box of
    // the plan's scissors as `PaintContext::cull`, so nodes that cannot reach any painted pixel
    // skip style resolution and emission. A full-redraw plan culls to the whole target (issue #257):
    // its sink stays unclipped, and what it drops cannot reach a pixel of the target, so the output
    // inside the viewport is unchanged. The full `paint_frame` overload (no plan) never culls.
    bool cull_paint = true;
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
    // Interpolated transform boxes left out of damage because they moved less than
    // `FramePipelineConfig::interpolation_epsilon_px` since they were last painted.
    std::size_t interpolation_suppressed = 0;
    // Glass nodes whose full box was added to damage because accumulated damage intersected their
    // 3 sigma dependency region (docs/specs/glass-v1.md section 7).
    std::size_t glass_dependencies = 0;
};

struct FramePaint {
    paint::PaintStats stats;
    bool painted = false;
};

// What one style sync last saw for a node (issue #701): the style's identity (its shared entries
// block), the style itself, whether it carried an `opacity`, and the sync generation that last
// visited it.
//
// The identity is compared first because it is a pointer test: a restyle that kept its entries
// cannot have changed the effective style. The stored style is the fallback for a *new* block --
// `SceneStyleTree::begin_memo` drops the memo on a non-incremental restyle, so a recompute produces
// a fresh block even when the values are identical, and only a value comparison can tell those
// apart. Storing it costs one copy per node per *generation*, not per frame: the copy is refreshed
// only when the block actually moved.
//
// `generation` replaces a separate `seen_` set and its per-frame `prune()` walk: a node the pass
// did not visit keeps an older stamp, and `prune` erases only those. The set-plus-erase form
// allocated once per node per frame (601 nodes x 120 frames = 72,120 allocations, 89% of a
// layout-dirty frame) because the erase/reinsert cycle rebuilt the map nodes.
struct StyleBaseline {
    std::shared_ptr<const void> identity;
    style::ComputedStyle style;
    std::uint64_t generation = 0;
    bool opacity_present = false;
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
    //
    // This is also where transition and animation declarations become real (issue #271): the same
    // walk that copies style onto the scene starts the CSS transitions whose properties changed and
    // reconciles the running CSS animations against the style's `animation-*` list. Both are handed
    // to the animation runtime, which the next tick samples, so the values reach layout and paint
    // through the ordinary overlay path. The web of `docs/specs/stylesheet-property-status-v1.md`
    // section 8 lists the longhands as applied through these two calls.
    //
    // The transition diff keeps one `ComputedStyle` snapshot per node that declares a transition
    // (docs/design/animation-runtime.md section 9), so a stylesheet with no transitions pays a
    // parse per node and nothing else. `keyframes` is the compiled sheet's table; pass the empty
    // vector for a sheet without animations and the animation half becomes a no-op.
    paint::SceneStyleSyncResult sync_style(Node& root, paint::StyleProvider& styles,
                                           const FrameTick& tick, bool force = false) const;

    // Scoped variant (issue #735, slice B): the same sync, but it visits only `roots` and their
    // subtrees plus the ancestors that refold `glass_subtree`, instead of the whole tree. The
    // caller must be able to prove every node outside `roots` kept its style AND its box - which is
    // exactly what the layout-unchanged gate proves, so the host calls this only on a frame the
    // gate accepted. An empty `roots` is a no-op.
    //
    // The transition/animation bridge still walks the whole tree: it diffs every node's style
    // against its previous snapshot, and a scoped walk there would need the same "unchanged"
    // proof per node. That is a separate slice.
    paint::SceneStyleSyncResult sync_style_scoped(Node& root, paint::StyleProvider& styles,
                                                  std::span<const Node* const> roots,
                                                  const FrameTick& tick, bool force = false) const;

    // The compiled sheet's `@keyframes`, for the animation half of `sync_style`. A pointer, not a
    // copy: the sheet outlives the pipeline (the host rebuilds the pipeline when it swaps sheets).
    // Null disables the animation half, which is what a test that only exercises transitions wants.
    void set_style_keyframes(const std::vector<style::KeyframeBlock>* keyframes) {
        style_keyframes_ = keyframes;
    }

    // Counters from the most recent `sync_style` (transitions and animations started or stopped);
    // refreshed by every call, so read it after `sync_style`.
    [[nodiscard]] const animation::StyleBridgeResult& style_bridge() const {
        return style_bridge_snapshot_;
    }

    // Paints the transitions and animations `sync_style` just started at their first value. They
    // start after this frame's tick, so without it the frame that applies a change paints the
    // destination style and the next one jumps back to the start (a one-frame flash). Samples the
    // new entries at zero elapsed time and marks their targets paint-dirty; a no-op when the last
    // `sync_style` started nothing or interpolation is off. Call before `finish_frame`.
    void sample_started_animations(Node& root);

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
    // A full-redraw plan forwards to the unclipped sink, so inside the target its commands are the
    // full paint above (its scissor list is ignored); with `cull_paint` the nodes that cannot reach
    // the target are skipped first (issue #257). An empty partial plan paints nothing, even
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
    // Adaptive quality's effects position (#147): paint optional effects in their cheaper form.
    // The flag is part of the paint-cache key, so a flip re-records instead of replaying stale
    // commands; the caller still requests a repaint because pixels change.
    void set_reduce_effects(bool reduce) {
        reduce_effects_ = reduce;
    }
    [[nodiscard]] bool reduce_effects() const {
        return reduce_effects_;
    }
    // High-contrast themes ignore `backdrop-filter` entirely (glass-v1 section 3.5): the
    // stylesheet's opaque fallback shows and the paint pass emits no glass draws. It is
    // accessibility behaviour, not the adaptive-quality decision of section 8, so it is a separate
    // flag from `reduce_effects`. The flag is part of the paint-cache key, so a flip re-records
    // instead of replaying a recorded backdrop draw.
    void set_high_contrast(bool high_contrast) {
        high_contrast_ = high_contrast;
    }
    [[nodiscard]] bool high_contrast() const {
        return high_contrast_;
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
    void tick_springs(Node& root, double dt, animation::AnimationTickResult& into);

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
    // Parsed `background-image` layer lists, keyed by style identity (issue #733 item 2). Mutable
    // and owned here for the same reason as the paint cache: it must outlive one frame for a
    // steady frame to stop re-parsing, and painting is const. A paint-cache hit never reaches the
    // gradient path, so this only serves the misses -- which is exactly where the parse was paid.
    mutable paint::BackgroundLayerCache background_layers_;
    PaintSamples previous_samples_;
    PaintSamples current_samples_;
    // Computed-style -> transitions/animations (issue #271). Mutable because `sync_style` is const
    // by contract (it resolves style, it does not change the frame decision) but the bridge has to
    // retain a per-node style snapshot between style stages.
    // Last computed style per node, the baseline `sync_style` diffs against so a restyle that
    // changes only paint properties still plans a redraw (dirty-rendering.md section 4.1).
    //
    // The identity is stored beside the style (issue #701): comparing identities is a pointer test,
    // and the stored style is only re-copied when the identity actually moved. Comparing values
    // against a stored copy allocated once per node per frame, because `begin_memo` drops the memo
    // each generation, so a recomputed style is a different object every time even when it is
    // equal.
    mutable std::unordered_map<NodeId, StyleBaseline> previous_styles_;
    // Bumped once per style sync; a baseline entry whose stamp is older was not visited and is
    // pruned. Replaces a per-sync `unordered_set` of seen ids (issue #701).
    mutable std::uint64_t style_sync_generation_ = 0;
    // Scratch for one style sync, retained so its capacity survives the frame (issue #701).
    mutable std::vector<std::pair<NodeId, bool>> style_diff_changed_;
    mutable animation::StyleBridge style_bridge_state_;
    // What the scene and interaction syncs applied last, so an unchanged node is not redone.
    mutable style::SyncMemo scene_sync_memo_;
    mutable style::SyncMemo interaction_sync_memo_;
    mutable animation::StyleBridgeResult style_bridge_snapshot_;
    const std::vector<style::KeyframeBlock>* style_keyframes_ = nullptr;
    // Painted corners (x0,y0..x3,y3, device px) of each transform-sampled node at the last frame
    // that added its box to damage; the reference the suppression distance is measured from.
    std::unordered_map<NodeId, std::array<float, 8>> last_damaged_corners_;
    // The ids one frame stage resolved this frame, as (id, node) pairs sorted by id (issue #735,
    // slice H). Retained so a steady frame reuses its capacity instead of allocating a map of the
    // whole tree; `node_lookup.hpp` owns the lookup over it.
    mutable std::vector<std::pair<NodeId, Node*>> node_lookup_buffer_;
    double alpha_ = 0.0;
    bool interpolate_ = false;
    bool reduce_effects_ = false;
    bool high_contrast_ = false;
};

} // namespace tessera::ui::frame
