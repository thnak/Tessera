// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstddef>
#include <memory>

namespace tessera::ui::text {
class TextMeasurer;
}

namespace tessera::app {

// Size and pacing knobs for one window host. Logical width/height are the window's client area in
// CSS pixels; `dpi_scale` converts them to the physical damage target. The root node is sized from
// `width`/`height` through host-owned inline style, so the stylesheet cannot resize the window.
struct HostConfig {
    float width = 800.0f;
    float height = 600.0f;
    float dpi_scale = 1.0f;
    std::size_t damage_max_rects = 8;
    // Layout text measurement. Null keeps the layout engine's deterministic stub (the goldens'
    // behaviour); a real app passes a measurer backed by the loaded font stack.
    std::shared_ptr<const ui::text::TextMeasurer> text_measurer;
    // The user's reduced-motion preference (the driver reads it from the OS). Animations snap to
    // their end value instead of moving; it applies to CSS animations, transitions and the
    // generated components' animated values.
    bool reduced_motion = false;
    ui::frame::FramePipelineConfig pipeline{};
};

// What one begin_frame() decided and did. `frame_due` mirrors FramePipeline: paint only when it is
// true (or when the caller forces a paint).
struct HostFrame {
    ui::frame::FrameTick tick;
    ui::frame::FrameRender render;
    ui::layout::LayoutApplyResult layout;
    ui::paint::SceneStyleSyncResult sync;
    bool style_rebuilt = false;
    bool frame_due = false;
};

// The production frame loop of docs/design/app-host.md section 3, steps 3-8: style, layout, scene
// sync, damage and paint for one scene root. The host owns the compiled stylesheet, the selector
// tree, the layout engine, the animation runtime, the dirty/damage state and the frame pipeline; it
// NEVER touches a window, swapchain or GPU device. The driver (a window runner or a headless test)
// owns the RuntimeLoop, drains input, calls begin_frame/paint/end_frame and presents.
//
// Per-frame contract:
//   frame = host.begin_frame(dt, alpha);   // tick + style + layout + sync + damage
//   if (frame.frame_due) { host.paint(sink); host.end_frame(); }
// A frame with no dirty nodes does no style, layout, paint or damage work, and paint() is a no-op
// unless the frame is due (or `force`). The selector tree is rebuilt whenever style or layout work
// is needed, because attaching a child changes the tree's shape (documented cost: one tree build
// per style/layout frame; it is skipped on idle and paint-only frames).
class AppHost final {
public:
    // `root` is borrowed and must outlive the host; it is the scene root (the window's content
    // node). The stylesheet is owned by value. `loop` is borrowed: the driver owns the runtime loop
    // and pacing. Throws std::invalid_argument for a non-finite/non-positive size or DPI scale.
    AppHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
            HostConfig config = {});
    ~AppHost();

    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;
    AppHost(AppHost&&) = delete;
    AppHost& operator=(AppHost&&) = delete;

    // Steps 3-6: animation tick, dirty collection, style resolution, Yoga layout, scene-style sync
    // (z-index/transform/box-shadow) and damage/redraw planning. `alpha` is the fixed-step
    // interpolation fraction for the render about to happen (0 paints the authoritative state).
    [[nodiscard]] HostFrame begin_frame(double dt, double alpha = 0.0);

    // Step 7: paints the scene through `sink` only when the last begin_frame() planned a frame, or
    // when `force` is set. The plan's scissor regions are applied, so a partial frame emits only
    // the damaged parts.
    [[nodiscard]] ui::frame::FramePaint paint(ui::paint::PaintSink& sink, bool force = false);

    // Step 8: clears every dirty domain on the scene after a painted frame. Safe to call after a
    // skipped frame too (nothing is dirty then).
    void end_frame();

    // Cumulative counters of the pipeline-owned per-node paint cache. begin_frame() invalidates the
    // cache whenever it rebuilds the selector tree (`HostFrame::style_rebuilt`, i.e. every frame
    // that reported style or layout work), because a rebuilt tree is a new style generation: a
    // clean node can compute a different style through inheritance, an ancestor/sibling selector or
    // a restyled root without its own dirty flag being set. A node's own style change marks it
    // dirty, which the cache key already handles. `resize()` re-creates the pipeline, so the cache
    // starts cold there. Read the counters after paint() to see one frame's hits/misses.
    [[nodiscard]] const ui::paint::PaintCacheStats& paint_cache_stats() const;

    // Client-area change: updates the root's inline size, the layout viewport, the damage target
    // and the pipeline's DPI scale, then marks the whole subtree for layout/paint and forces a
    // full redraw (the old target's damage rectangles are meaningless at the new size).
    void resize(float width, float height, float dpi_scale);

    // Latches a full-target repaint for the next begin_frame(). Use it when the damage/redraw state
    // cannot be trusted to describe what is on screen (the first frame after a pause, an occlusion
    // or a display switch): the next frame's plan is a full redraw regardless of the damage
    // accumulated since, so the paint covers every pixel. The latch is one-shot and survives no
    // frame that paints; the runtime loop is woken so the frame is produced on demand.
    void request_full_repaint();

    // Installs the per-window update scheduler. Its flush runs before animation, style and layout
    // of every begin_frame(), so generated components' schedule_update() tickets apply in the frame
    // that asked for them. Pass nullptr to uninstall; the scheduler must outlive the host or be
    // detached first.
    void set_update_scheduler(ui::UpdateScheduler* scheduler);

    // Changes the reduced-motion preference at run time (the OS setting changed). Forwarded to the
    // animation runtime and the installed scheduler; a scheduler installed later inherits it.
    void set_reduced_motion(bool reduced);
    [[nodiscard]] bool reduced_motion() const;

    [[nodiscard]] bool frame_due() const;
    [[nodiscard]] const ui::frame::FrameRender& last_render() const;
    [[nodiscard]] ui::paint::StyleProvider& styles();
    [[nodiscard]] ui::layout::LayoutEngine& layout_engine();
    // Layout passes actually run, and cached results reused because the layout input signature
    // (tree shape, layout-affecting longhands, text, viewport) did not change.
    [[nodiscard]] std::size_t layout_runs() const;
    [[nodiscard]] std::size_t layout_cache_hits() const;
    [[nodiscard]] ui::animation::AnimationRuntime& animations();
    // Style-property springs, owned and ticked by the frame pipeline (see FramePipeline::springs).
    [[nodiscard]] ui::animation::SpringRuntime& springs();
    [[nodiscard]] float width() const;
    [[nodiscard]] float height() const;
    [[nodiscard]] float dpi_scale() const;
    // Physical pixels of the damage/present target.
    [[nodiscard]] rendering::rhi::Extent2D target_extent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::app
