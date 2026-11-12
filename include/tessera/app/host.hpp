// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/config.hpp"
#include "tessera/runtime/diagnostics.hpp"
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
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::text {
class TextMeasurer;
}

namespace tessera::app {

// The adaptive-quality policy of one host window, a plain mirror of the runtime-config keys
// (`adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale`, `frameBudgetMs`) in
// docs/specs/runtime-config-v1.md sections 3, 5.6 and 5.7. The decision layer itself lives in the
// runtime library; the host builds it from these fields once, at construction.
//
// `render_scale` is the full-quality scale and `min_render_scale` the floor of the adaptive
// ladder. `target_fps` is the effective present rate used to derive the budget when
// `frame_budget_ms` is 0 (0 = no budget, so nothing is ever observed as slow).
//
// `max_fps` and `refresh_hz` feed the present-cap component (step 3): the cap is the configured
// `max_fps` at step 0 and the next refresh divisor below the previous step's effective rate after
// each downgrade. `max_fps` is a hard ceiling, so the adaptive cap can only lower the rate; a
// `refresh_hz` of 0 (unknown display) leaves the cap at `max_fps` and the pacer on deadlines.
struct AdaptiveQualityPolicy {
    bool adaptive_quality = true;
    PowerPolicy power_policy = PowerPolicy::Adaptive;
    float render_scale = 1.0f;
    float min_render_scale = 0.6f;
    double frame_budget_ms = 0.0;
    double target_fps = 60.0;
    double max_fps = 0.0;
    double refresh_hz = 0.0;

    [[nodiscard]] bool operator==(const AdaptiveQualityPolicy&) const = default;
};

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
    // Adaptive quality (docs/design/performance.md section 6). The defaults match
    // RuntimeConfig{}, so a host that never configures it behaves exactly like one that does.
    AdaptiveQualityPolicy adaptive_quality{};
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

    // --- Adaptive quality (docs/design/performance.md section 6) ------------------------------
    // Feeds one rendered frame's measured CPU time in milliseconds to the deterministic controller
    // and records the decision in the host's frame diagnostics. The driver calls it once per frame
    // it actually presented, after the present; a frame that was skipped is not an observation. A
    // non-finite or non-positive time carries no measurement and is ignored. The call allocates
    // nothing and is one state-machine step, so it belongs on the frame path.
    //
    // What it does NOT do: it never resizes a render target, never changes the damage extent and
    // never touches paint. The scene is rendered at the window's physical size exactly as before;
    // `adaptive_quality().render_scale` is a reported decision whose render-target consumer is a
    // follow-up slice (docs/specs/runtime-config-v1.md section 6.2).
    void observe_frame_time_ms(double frame_time_ms);

    // The adaptive-quality decision in force (full quality until a policy actually changes it).
    [[nodiscard]] const runtime::AdaptiveQualitySample& adaptive_quality() const;

    // Frame diagnostics accumulated by this host: the latest adaptive-quality sample plus the
    // change/downgrade counters. Reset only by constructing a new host; a downgrade stays visible
    // for the host's lifetime instead of disappearing after one frame.
    [[nodiscard]] const runtime::FrameDiagnostics& frame_diagnostics() const;

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

    // Installs the app's embedded stylesheets (issue #81, ADR-048) once, before frames. Each blob
    // is loaded with `load_stylesheet_blob` — never parsed — and combined with the host's
    // constructor sheet, which keeps the lowest source order, in `sheets` (import-graph) order, so
    // a later sheet wins an otherwise equal cascade key. The host keeps the constructor sheet, so
    // calling this again replaces the previous registration instead of stacking on it, and marks
    // style, layout, paint and render dirty so the next begin_frame() rebuilds the selector tree;
    // begin_frame() never parses CSS. A blob that fails to load never throws and never applies: the
    // sheet is skipped, one `TESSERA_CSS_BLOB_INVALID: <path>: <code>: <message>` line goes to
    // stderr, the remaining sheets still register, and rejected_stylesheet_count() reports the
    // skipped count. Library CSS reaches the cascade already layered in `@layer tessera-ui` (build
    // the constructor sheet from `style::StylesheetSource` with `library = true`).
    void register_embedded_stylesheets(std::span<const ui::style::EmbeddedStylesheet> sheets);

    // Sheets skipped by the most recent register_embedded_stylesheets() call.
    [[nodiscard]] std::size_t rejected_stylesheet_count() const;

    // Installs the project's embedded theme asset (issue #81, ADR-048) once, before frames. The
    // blob is loaded with `load_theme_asset_blob` — the asset JSON is never parsed at run time —
    // and the asset's `start` theme becomes active. Calling this again replaces the asset. A blob
    // that fails to load never throws and never applies: one
    // `TESSERA_THEME_BLOB_INVALID: <code>: <message>` line goes to stderr, the current theme is
    // left untouched, and rejected_theme_count() reports it.
    void register_embedded_theme(std::span<const std::uint8_t> bytes);

    // Activates one theme id of the installed asset; false (and no change) for an unknown id.
    // Only that theme's tokens are injected: the host rebuilds the cascade with the generated
    // `:root` rule, marks style, paint and render dirty, rebuilds the selector tree and drops the
    // paint cache, so the next begin_frame() re-resolves styles. Wakes the runtime loop.
    bool set_theme(std::string_view id);

    // Theme ids of the installed asset: `default` first, then the `themes` keys in asset order.
    [[nodiscard]] std::vector<std::string> theme_ids() const;
    [[nodiscard]] const std::string& active_theme() const;
    // The value the active theme injects for `name`. A reserved chrome token the theme leaves out
    // reports the built-in value (stylesheet-v1.md section 9.2); anything else is nullopt.
    [[nodiscard]] std::optional<std::string_view> theme_token(std::string_view name) const;
    // Theme blobs rejected by the most recent register_embedded_theme() call.
    [[nodiscard]] std::size_t rejected_theme_count() const;

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
