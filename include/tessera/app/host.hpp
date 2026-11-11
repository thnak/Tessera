// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
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

    // Client-area change: updates the root's inline size, the layout viewport, the damage target
    // and the pipeline's DPI scale, then marks the whole subtree for layout/paint and forces a
    // full redraw (the old target's damage rectangles are meaningless at the new size).
    void resize(float width, float height, float dpi_scale);

    [[nodiscard]] bool frame_due() const;
    [[nodiscard]] const ui::frame::FrameRender& last_render() const;
    [[nodiscard]] ui::paint::StyleProvider& styles();
    [[nodiscard]] ui::layout::LayoutEngine& layout_engine();
    [[nodiscard]] ui::animation::AnimationRuntime& animations();
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
