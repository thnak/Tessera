// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_observer.hpp"
#include "tessera/ui/layout/layout_observer_settle.hpp"
#include "tessera/ui/overlay/overlay.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/responsive/environment_set.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstddef>
#include <cstdint>
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
    // The OS appearance the host starts with (issue #187): the color scheme and reduced-motion
    // preferences the platform reported at window creation. The color scheme selects the `@media
    // (prefers-color-scheme: ...)` rules the stylesheet is resolved against, and the motion flag is
    // the initial `reduced_motion` above (a caller that sets `reduced_motion` explicitly keeps it:
    // the flag here only fills the media context and is not a second source of truth for motion).
    hal::AppearancePreference appearance{};
    ui::frame::FramePipelineConfig pipeline{};
    // Adaptive quality (docs/design/performance.md section 6). The defaults match
    // RuntimeConfig{}, so a host that never configures it behaves exactly like one that does.
    AdaptiveQualityPolicy adaptive_quality{};
    // Same-tick settle rounds for layout observation (ADR-041, `layoutObservationMaxRounds` in
    // docs/specs/runtime-config-v1.md section 3). 1 means next-tick delivery; the range is 1..16
    // and an out-of-range value clamps. It only costs anything when the stylesheet has `@container`
    // rules or a component observes its size.
    std::uint32_t layout_observation_max_rounds = 4;
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
// What the host did with the last BackRequested input (docs/specs/studio-device-profiles-v1.md
// section 4): the topmost dismissible overlay, else the app's back handler (a route pop), else
// nothing took it. `Unhandled` is the result the embedding host acts on: a desktop host ignores
// it, a mobile host closes the app.
enum class BackDispatch : std::uint8_t {
    None, // no BackRequested has reached the host yet
    OverlayDismissed,
    HandlerConsumed,
    Unhandled,
};

// The app's route-level Back: returns true when it consumed the request (a route or modal was
// popped). Plain function pointer plus context, so the input path allocates nothing.
using BackHandler = bool (*)(void* user);

class AppHost final {
public:
    // `root` is borrowed and must outlive the host; it is the scene root (the window's content
    // node). The stylesheet is owned by value. `loop` is borrowed: the driver owns the runtime loop
    // and pacing. Throws std::invalid_argument for a non-finite/non-positive size or DPI scale.
    AppHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
            HostConfig config = {});
    // The re-resolvable form (issue #187, ADR-048 decision 2): a media-independent prepared sheet
    // plus the media context to resolve it against. `@media` is evaluated from `config.appearance`,
    // and a later appearance change re-resolves the SAME prepared data, which is the documented way
    // a color-scheme change is applied. A caller that has only a compiled sheet uses the overload
    // above, where a color-scheme change logs that it cannot re-resolve instead of silently
    // ignoring it.
    AppHost(ui::Node& root, const ui::style::PreparedStyleSheet& laid_out_sheet,
            runtime::RuntimeLoop& loop, HostConfig config,
            ui::style::Origin origin = ui::style::Origin::Author);
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

    // Hands the host the window metrics SafeArea nodes read (docs/specs/safe-area-v1.md): insets,
    // cutouts, keyboard and hinge. The first call and reduced motion apply them at once; later
    // ones animate each inset edge independently over the next frames. A node with a safe-area spec
    // gets its padding or margin from the result before layout. Cheap when nothing changed.
    //
    // A snapshot that reports a usable logical extent which differs from the host's size is also a
    // size change (issue #436): it goes through the same path `resize()` does, media context
    // included, so a driver that feeds only metrics cannot leave `@media` resolved against the
    // previous rectangle. A snapshot with no usable extent (insets only) moves the safe area alone.
    void set_window_metrics(const hal::WindowMetrics& metrics);

    // The window's default OverlayHost (docs/specs/overlays-and-layers-v1.md, issue #302), created
    // on first use so a scene that never opens an overlay gains no nodes. Requests recorded on it
    // apply at the start of the next begin_frame(); prefer open_overlay()/close_overlay(), which
    // also wake an idle loop for that frame.
    [[nodiscard]] ui::OverlayHost& overlays();
    ui::OverlayHandle open_overlay(ui::OverlaySpec spec);
    bool close_overlay(ui::OverlayHandle handle);
    // The overlay side of the input path: Escape dismisses the topmost dismissible overlay and is
    // consumed (true: the caller must not dispatch it), a press outside closes the overlays that
    // opted in and still reaches the scene, and window blur closes the transient ones. The window
    // installs it as the input pump's filter; headless drivers call it before dispatching.
    bool handle_overlay_input(const ui::InputEvent& event);

    // Back dispatch order: topmost dismissible overlay, else this handler, else unhandled. A null
    // handler means the app has no route stack, so Back is overlay-or-unhandled.
    void set_back_handler(BackHandler handler, void* user = nullptr);
    // The outcome of the most recent BackRequested, and how many reached `Unhandled` since the
    // host was created. Both are plain counters: reading them never blocks or allocates.
    [[nodiscard]] BackDispatch last_back_dispatch() const;
    [[nodiscard]] std::size_t unhandled_back_count() const;

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

    // GPU-aware observation (docs/design/performance.md section 6.1). `gpu_frame_time_ms` is the
    // completed submission's GPU interval from the RHI timestamp probe; when it is present and
    // plausible the controller and diagnostics use it, otherwise the CPU sample is used exactly as
    // observe_frame_time_ms() does. Which source produced the observation is recorded in
    // FrameDiagnostics. Callers must not invent a GPU number: only a value read through
    // CommandList::gpu_frame_time_ms() after the slot completed belongs here.
    void observe_frame_time_sample(double cpu_frame_time_ms,
                                   std::optional<double> gpu_frame_time_ms);

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
    // What the generic layer promotion policy currently promotes (issue #1024: the overlay line).
    [[nodiscard]] ui::paint::LayerPromotionStats layer_promotion_stats() const;

    // Client-area change: updates the root's inline size, the layout viewport, the damage target
    // and the pipeline's DPI scale, then marks the whole subtree for layout/paint and forces a
    // full redraw (the old target's damage rectangles are meaningless at the new size).
    //
    // Since issue #436 it also moves the media context the stylesheet is resolved against: the
    // logical size becomes `viewport_width/height` and the DPI scale becomes `resolution_dpi` (96
    // dpi at scale 1), so `@media (min-width|max-width|orientation|resolution)` and the viewport
    // units follow the window. The cascade is re-resolved **only when a media answer can actually
    // change**
    // - when a breakpoint is crossed - so a drag-resize that crosses none costs one layout pass and
    // no cascade rebuild. `cascade_generation()` is the counter that distinguishes the two.
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

    // The window's layout-observation set (ADR-041, issue #693). Generated components attach their
    // `LayoutObservation`s here at mount; the host publishes the committed layout after every
    // layout pass and delivers the changed sizes once per tick, in bounded settle rounds, before
    // damage and paint. Empty by default, and a host whose sheet has no `@container` and whose
    // scene attaches nothing pays one boolean test per frame.
    [[nodiscard]] ui::layout::LayoutObserverSet& layout_observers();

    // The window's environment-value set (docs/specs/jsx-subset-v1.md section 7.1, issue #692).
    // Generated components register their `set_environment_<name>()` here at mount; the host pushes
    // the current size class after every size change, so `useBreakpoint()` and the stylesheet's own
    // `@media` answer come from the same `MediaContext` and cannot disagree. Empty by default, and
    // a component that reads no environment value registers nothing.
    [[nodiscard]] ui::responsive::EnvironmentSet& environment_values();

    // The size class in force (the same answer `useBreakpoint()` returns). Read from the host's
    // media context, so a test can assert the hook and the stylesheet agree.
    [[nodiscard]] ui::responsive::SizeClass size_class() const;

    // The settle loop's counters (ADR-041 section 5.3): deliveries, rounds, engine rounds, deferred
    // observations and round-limit hits. They are how "a resize inside one condition costs a layout
    // pass and nothing else" is asserted by a counter instead of inferred from a picture.
    [[nodiscard]] const ui::layout::SettleTotals& layout_settle_totals() const;

    // Changes the reduced-motion preference at run time (the OS setting changed). Forwarded to the
    // animation runtime and the installed scheduler; a scheduler installed later inherits it.
    void set_reduced_motion(bool reduced);
    [[nodiscard]] bool reduced_motion() const;

    // Applies a new OS appearance snapshot at run time (issue #187): the reduced-motion flag goes
    // through `set_reduced_motion`, and a changed color scheme re-resolves the stylesheet against
    // the new `prefers-color-scheme` value, which is the same path a theme switch takes (the
    // cascade is rebuilt, every node is marked, the paint cache is dropped and the loop wakes). The
    // call is a no-op when neither preference moved, so a settings broadcast that changed something
    // else costs nothing. The media context the host resolves against is also readable.
    void set_appearance(const hal::AppearancePreference& appearance);
    [[nodiscard]] const ui::style::MediaContext& media_context() const;
    // How many times the cascade has been resolved: 1 after construction, +1 for every rebuild
    // (a theme switch, an appearance change, an embedded-sheet registration, and a resize that
    // crosses a `@media` breakpoint - issue #436). It is the counter that separates "the cascade
    // was re-resolved" from "the boxes were laid out again", which is the difference between a
    // responsive stylesheet and one that never sees the window.
    [[nodiscard]] std::uint64_t cascade_generation() const;

    [[nodiscard]] bool frame_due() const;
    [[nodiscard]] const ui::frame::FrameRender& last_render() const;
    [[nodiscard]] ui::paint::StyleProvider& styles();
    [[nodiscard]] ui::layout::LayoutEngine& layout_engine();
    // Layout passes actually run, and cached results reused because the layout input signature
    // (tree shape, layout-affecting longhands, text, viewport) did not change.
    [[nodiscard]] std::size_t layout_runs() const;
    [[nodiscard]] std::size_t layout_cache_hits() const;
    // Frames whose layout pass the layout-unchanged gate skipped entirely (issue #735, slice A):
    // the restyle provably kept every layout input, so the previous result was reused without
    // building a layout input or hashing the tree. A subset of `layout_cache_hits()`'s story told
    // from the other side: those are passes that ran and hit the signature cache, these are passes
    // that never started.
    [[nodiscard]] std::size_t layout_gate_hits() const;
    // Why the layout-unchanged gate declined the most recent frame, as a stable name ("None" when
    // it accepted). Diagnostics and tests only; the frame path never reads it.
    [[nodiscard]] const char* layout_gate_reason() const;
    [[nodiscard]] ui::animation::AnimationRuntime& animations();
    // The one full-scene paint every driver uses (the window driver and the frame capture): it
    // paints the scene through the host's style provider and sets the tick's sampled transition
    // and animation values as `context.view`. Two drivers each calling `paint::paint_scene` was
    // how a transition could start, tick and never reach the pixels: one of them forgot the view.
    void paint_scene(ui::paint::PaintSink& sink, ui::paint::PaintContext context = {});
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
