// SPDX-License-Identifier: MIT

#include "host_impl.hpp"
#include "tessera/app/host.hpp"
#include "tessera/hal/safe_area.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/scene/text_wrap.hpp"
#include "tessera/ui/style/ua_stylesheet.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::app {
namespace {

constexpr float kMinExtent = 1.0f;
constexpr float kMaxExtent = 65535.0f;

float checked_extent(float value, const char* name) {
    if (!std::isfinite(value) || value < kMinExtent || value > kMaxExtent) {
        throw std::invalid_argument(std::string("app host: invalid ") + name);
    }
    return value;
}

float checked_scale(float value) {
    if (!std::isfinite(value) || value <= 0.0f || value > 8.0f) {
        throw std::invalid_argument("app host: invalid dpi scale");
    }
    return value;
}

// Formats `<value>px` into caller-owned storage. The scene borrows inline-style text
// (`Node::set_inline_style` keeps a `string_view`), so the host must own the generated sizes for as
// long as the root carries them. A temporary here leaves the root's computed `width`/`height`
// reading freed memory as soon as the style tree is rebuilt, which makes the layout input - and so
// the layout signature - differ from frame to frame even though nothing was edited.
void write_pixels(std::array<char, 32>& buffer, float value) {
    std::snprintf(buffer.data(), buffer.size(), "%gpx", static_cast<double>(value));
}

// Marks every SafeArea node for layout: their padding or margin comes from the host's insets, which
// no style or scene mutation reports.
void mark_safe_area_nodes(ui::Node& node) {
    if (ui::style::is_safe_area_node(node)) {
        node.mark_dirty(ui::Dirty::Layout);
    }
    for (const auto& child : node.children()) {
        mark_safe_area_nodes(*child);
    }
}

// `mark_subtree()` lives in host_viewport.cpp beside `extent_for()`: both are shared with the
// viewport half of a resize, and this file is over its size budget. `build_layout_input()` and
// `run_layout_pass()` live in host_layout_gate.cpp for the same reason.

bool has_inline_style(const ui::Node& node, std::string_view name) {
    for (const ui::Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        if (declaration.name == name) {
            return true;
        }
    }
    return false;
}

// The public policy struct and the runtime's private controller config carry the same six fields;
// this is the one place that converts between them, so a new key is added once.
runtime::AdaptiveQualityConfig quality_controller_config(const AdaptiveQualityPolicy& policy) {
    runtime::AdaptiveQualityConfig config;
    config.adaptive_quality = policy.adaptive_quality;
    config.power_policy = policy.power_policy;
    config.render_scale = policy.render_scale;
    config.min_render_scale = policy.min_render_scale;
    config.frame_budget_ms = policy.frame_budget_ms;
    config.target_fps = policy.target_fps;
    config.max_fps = policy.max_fps;
    config.refresh_hz = policy.refresh_hz;
    return config;
}

} // namespace

AppHost::Impl::Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
                    HostConfig config)
    : root(root),
      base_sheet(std::move(sheet)),
      sheet(base_sheet),
      loop(loop),
      config(config),
      engine(config.text_measurer),
      damage(std::make_unique<rendering::DamageAccumulator>(
          rendering::DamageConfig{detail::extent_for(config.width, config.height, config.dpi_scale),
                                  config.damage_max_rects})),
      // A decision either changes pixels (reduced effects, so a full repaint covers it) or stops
      // animations (so resuming has to wake an idle loop). Both rules live in the stage.
      quality(detail::QualityStageContext{&scene_has_blurred_shadow_callback,
                                          &non_essential_animations_callback,
                                          &set_reduce_effects_callback,
                                          &set_animations_paused_callback, &frame_diagnostics,
                                          this},
              quality_controller_config(config.adaptive_quality)) {
    // The OS appearance the window reported at creation (issue #187), and the media context the
    // very first cascade resolves against. The context is the WINDOW, not a default rectangle
    // (issue #436): `@media (max-width: 600px)` and the viewport units must see the size the app
    // starts at, so `viewport_width/height` are the logical client area and `resolution_dpi` the
    // DPI scale. Both must be in place BEFORE the first cascade: they are resolved there, once per
    // generation.
    appearance = config.appearance;
    media_context = window_media_context(config.width, config.height, config.dpi_scale, appearance);
    // A caller that set `reduced_motion` explicitly keeps it; otherwise the OS flag is the answer.
    // Keeping one source of truth avoids a host whose animations and media queries disagree.
    if (config.reduced_motion) {
        appearance.reduced_motion = true;
    }
    reduced_motion = appearance.reduced_motion;
    // The window root is the host's geometry: logical size plus the column flex container every
    // app content area starts from. Inline style is the outermost origin, so an app stylesheet
    // can style the root's background but never resize the window.
    apply_root_size();
    // A project without a theme asset still has the built-in `default` theme, so the reserved
    // chrome pair and `var(--token)` behave the same with and without an asset.
    theme_asset = ui::style::default_theme_asset();
    active_theme_id = theme_asset.start;
    rebuild_cascade();
    tree = std::make_unique<ui::style::SceneStyleTree>(root);
    provider = std::make_unique<ui::paint::SceneStyleProvider>(this->sheet, *tree);
    pipeline = std::make_unique<ui::frame::FramePipeline>(animations, this->loop, tracker, *damage,
                                                          dirty, this->config.pipeline);
    // `@keyframes` for the animation half of the style bridge. The pipeline borrows the vector, so
    // the pointer is refreshed after every rebuild_cascade() that replaces `sheet` (issue #271).
    pipeline->set_style_keyframes(&this->sheet.keyframes());
    // Publish the full-quality decision before the first frame, so diagnostics and
    // adaptive_quality() agree from the start instead of reporting a default 1.0 scale for a window
    // configured at another renderScale.
    quality.publish();
    // The ADR-041 settle loop (issue #693): its hooks and round budget live in host_settle.cpp.
    init_settle();
    // The size class the first frame starts at (issue #692): a component that reads
    // `useBreakpoint()` renders the class the window actually has, not the field's initial value.
    pushed_size_class = ui::responsive::size_class_for(media_context);
}

void AppHost::Impl::apply_root_size() {
    // Window geometry is host-owned, but the root's layout mode is not: a shell with a sidebar
    // sets its own `display`/`flex-direction` before the host exists, and clobbering it would
    // silently stack the shell. Only fill in the defaults the caller did not choose.
    //
    // The size strings live in host-owned buffers because the scene borrows them (see
    // write_pixels); they are stable for the host's lifetime, so a resize re-formats in place
    // and refreshes the declarations without invalidating anything already stored.
    write_pixels(root_width_text_, config.width);
    write_pixels(root_height_text_, config.height);
    root.set_inline_style("width", std::string_view(root_width_text_.data()));
    root.set_inline_style("height", std::string_view(root_height_text_.data()));
    if (!has_inline_style(root, "display")) {
        root.set_inline_style("display", "flex");
    }
    if (!has_inline_style(root, "flex-direction")) {
        root.set_inline_style("flex-direction", "column");
    }
    // The size text is rewritten in place, so the scene sees no edit and marks nothing Style-dirty:
    // the memoised root style (and what inherits from it) is stale until the next full restyle.
    style_tree_populated = false;
}

void AppHost::Impl::rebuild_style_tree() {
    // Attaching/removing children invalidates the captured tree (SceneStyleTree), and the
    // provider borrows it, so both are recreated together.
    tree = std::make_unique<ui::style::SceneStyleTree>(root);
    provider = std::make_unique<ui::paint::SceneStyleProvider>(sheet, *tree);
    style_tree_populated = false;
}

// One cascade: the constructor sheet (lowest source order), the embedded sheets in table order,
// and the active theme's generated `:root` rule last with `Origin::Theme` (stylesheet-v1.md
// section 9.1). The theme rule cannot win an author declaration of the same token because the
// theme origin ranks below author, and only the active theme is present.
//
// Every sheet is resolved against `media_context` (issue #187): `@media (prefers-color-scheme)`,
// `(prefers-reduced-motion)` and - since issue #436 - the viewport itself are evaluated here, once
// per cascade generation, not per frame. So an appearance change, a theme switch and a resize that
// crosses a breakpoint all go through this one rebuild.
void AppHost::Impl::rebuild_cascade() {
    // One generation per resolved cascade; `AppHost::cascade_generation()` is how a test tells a
    // re-resolve from a layout pass (issue #436).
    ++cascade_generation;
    // The re-resolvable form (issue #187, ADR-048 decision 2): the prepared sheet is media
    // independent, so re-resolving the SAME data against a new `MediaContext` is how a color-scheme
    // change is applied. `base_sheet` is the compiled copy that this then replaces.
    if (base_prepared) {
        ui::style::CompiledStyleSheet combined(*base_prepared, base_origin, {}, media_context);
        // The user-agent defaults ride in every cascade; their origin ranks below all others, so
        // the position here does not matter and an author declaration always wins.
        combined.append_sheet(ui::style::ua_prepared_stylesheet(), ui::style::Origin::UserAgent);
        for (const ui::style::PreparedStyleSheet& prepared : embedded_sheets) {
            combined.append_sheet(prepared);
        }
        combined.append_sheet(ui::style::prepare_theme_stylesheet(theme_asset, active_theme_id),
                              ui::style::Origin::Theme);
        sheet = std::move(combined);
        return;
    }
    ui::style::CompiledStyleSheet combined = base_sheet;
    combined.append_sheet(ui::style::ua_prepared_stylesheet(), ui::style::Origin::UserAgent);
    for (const ui::style::PreparedStyleSheet& prepared : embedded_sheets) {
        combined.append_sheet(prepared);
    }
    combined.append_sheet(ui::style::prepare_theme_stylesheet(theme_asset, active_theme_id),
                          ui::style::Origin::Theme);
    sheet = std::move(combined);
}

// A theme or sheet generation change: every node can resolve different tokens through inheritance,
// so the whole subtree is marked and the style tree and paint cache are rebuilt. The caller
// chooses whether layout is marked: a stylesheet can change any property, while a theme switch
// only changes tokens (a token used by a layout property still re-resolves because begin_frame()
// runs the layout pass whenever style work is due).
void AppHost::Impl::apply_style_generation(ui::Dirty dirty_mask) {
    rebuild_cascade();
    pipeline->set_style_keyframes(&this->sheet.keyframes());
    rebuild_style_tree();
    pipeline->invalidate_paint_cache();
    detail::mark_subtree(root, dirty_mask);
    loop.mark_render_dirty();
}

// The window minus every inset an overlay must keep clear of: bars, cutout, keyboard, gesture
// zones and hinge (safe-area-v1.md). Before the first metrics arrive the whole window is usable.
rendering::rhi::Rect AppHost::Impl::overlay_viewport() const {
    rendering::rhi::Rect viewport{0.0f, 0.0f, config.width, config.height};
    if (!safe_area.has_metrics()) {
        return viewport;
    }
    hal::SafeAreaConfig keep_clear;
    keep_clear.sources = hal::kSourcesDefault | hal::kSourceGestures;
    const hal::WindowInsets insets = safe_area.resolve(keep_clear);
    viewport.x = insets.left;
    viewport.y = insets.top;
    viewport.width = std::max(0.0f, config.width - insets.left - insets.right);
    viewport.height = std::max(0.0f, config.height - insets.top - insets.bottom);
    return viewport;
}

bool AppHost::Impl::root_is_rtl() const {
    const std::string* direction = provider->compute(root).find("direction");
    return direction != nullptr && *direction == "rtl";
}

bool AppHost::handle_overlay_input(const ui::InputEvent& event) {
    Impl& state = *impl_;
    if (event.type == ui::InputEventType::BackRequested) {
        return state.handle_back_request();
    }
    if (!state.overlay_host) {
        return false;
    }
    ui::OverlayHost& overlays = *state.overlay_host;
    switch (event.type) {
    case ui::InputEventType::KeyDown:
        if (event.key == ui::key::kEscape && overlays.handle_escape()) {
            state.escape_swallowed = true;
            state.loop.mark_render_dirty();
            return true;
        }
        return false;
    case ui::InputEventType::KeyUp:
        // The release that belongs to a consumed press must not reach a focused control alone.
        if (event.key == ui::key::kEscape && state.escape_swallowed) {
            state.escape_swallowed = false;
            return true;
        }
        return false;
    case ui::InputEventType::PointerDown:
        // The press still reaches whatever is under it; only the overlays close.
        if (overlays.handle_pointer_down(event.x, event.y) > 0) {
            state.loop.mark_render_dirty();
        }
        return false;
    case ui::InputEventType::Blur:
        if (overlays.close_transient() > 0) {
            state.loop.mark_render_dirty();
        }
        return false;
    default:
        return false;
    }
}

AppHost::AppHost(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
                 HostConfig config)
    : impl_(nullptr) {
    // Validate before the implementation builds anything from the size.
    config.width = checked_extent(config.width, "width");
    config.height = checked_extent(config.height, "height");
    config.dpi_scale = checked_scale(config.dpi_scale);
    impl_ = std::make_unique<Impl>(root, std::move(sheet), loop, config);
    impl_->reduced_motion = config.reduced_motion;
    impl_->animations.set_reduced_motion(impl_->reduced_motion);
}

// The re-resolvable constructor (issue #187): the prepared sheet is kept so a later color-scheme
// change re-resolves the same data against the new media context instead of re-parsing CSS.
AppHost::AppHost(ui::Node& root, const ui::style::PreparedStyleSheet& laid_out_sheet,
                 runtime::RuntimeLoop& loop, HostConfig config, ui::style::Origin origin)
    : impl_(nullptr) {
    config.width = checked_extent(config.width, "width");
    config.height = checked_extent(config.height, "height");
    config.dpi_scale = checked_scale(config.dpi_scale);
    // Resolve the prepared data once with the startup appearance AND the startup window (issue
    // #436: a host that starts at 390x844 must resolve `@media (max-width: 600px)` on its first
    // cascade, not when it is first resized), then keep the prepared data for every rebuild.
    ui::style::CompiledStyleSheet resolved(
        laid_out_sheet, origin, {},
        window_media_context(config.width, config.height, config.dpi_scale, config.appearance));
    impl_ = std::make_unique<Impl>(root, std::move(resolved), loop, config);
    impl_->base_prepared = laid_out_sheet;
    impl_->base_origin = origin;
    impl_->reduced_motion = config.reduced_motion || config.appearance.reduced_motion;
    impl_->animations.set_reduced_motion(impl_->reduced_motion);
}

AppHost::~AppHost() {
    // The root outlives the host (capture_frame reuses one scene across hosts) but its width and
    // height declarations borrow this host's buffers: take them back before they die.
    if (impl_) {
        impl_->root.remove_inline_style("width");
        impl_->root.remove_inline_style("height");
    }
}

HostFrame AppHost::begin_frame(double dt, double alpha) {
    Impl& state = *impl_;
    HostFrame frame;
    // Overlay opens and closes land on the frame boundary, before anything reads the tree.
    if (state.overlay_host) {
        state.overlay_host->flush();
    }
    // Insets ease before the tick so the layout of this frame already sees the new values; an
    // animation in flight keeps the loop awake, a settled one costs nothing.
    if (state.safe_area.step(dt, state.reduced_motion)) {
        mark_safe_area_nodes(state.root);
    }
    if (state.safe_area.animating()) {
        state.loop.mark_render_dirty();
    }
    frame.tick = state.pipeline->tick(state.root, dt, alpha);

    // A latched full repaint is re-applied after the tick: tick() starts from a cleared damage
    // accumulator, so latching before it would be dropped. finish_frame() below turns it into a
    // full-target plan.
    if (state.full_repaint_pending) {
        state.damage->force_full_redraw();
        state.full_repaint_pending = false;
    }

    // Any style or layout work may have changed the tree's shape (a state change can attach a
    // conditional child), and the damage pass latches a full redraw on overflow. Rebuild the
    // selector tree and re-run layout for both; a paint-only frame skips all of it. ADR-041 steps
    // 4a/4b (issue #693) follow every pass: publish the committed layout and run the bounded settle
    // loop, so a container query or an `onSizeChange` callback that changes layout lands in THIS
    // frame instead of the next one.
    const auto restyle_and_layout = [&state, &frame] {
        state.restyle_and_layout(frame.tick, frame.sync, frame.style_rebuilt);
        frame.layout = state.last_applied;
        state.settle_layout();
    };
    const bool geometry_work = frame.tick.needs_style || frame.tick.needs_layout;
    if (geometry_work) {
        restyle_and_layout();
    }
    // Placed overlays need the content size and the anchor bounds, so they position after layout;
    // only a content box that actually moved costs a second pass.
    if (state.overlay_host && state.overlay_host->has_placed()) {
        const auto placed =
            state.overlay_host->place(state.overlay_viewport(), state.root_is_rtl());
        if (placed.closed) {
            state.loop.mark_render_dirty(); // the closing entry unmounts on the next frame
        }
        if (placed.moved) {
            restyle_and_layout();
        }
    }

    frame.render = state.pipeline->finish_frame(state.root);
    frame.frame_due = state.pipeline->frame_due();
    state.last_render = frame.render;
    return frame;
}

ui::frame::FramePaint AppHost::paint(ui::paint::PaintSink& sink, bool force) {
    Impl& state = *impl_;
    const rendering::RedrawPlan& plan = state.last_render.redraw_plan;
    if (force && !plan.full_redraw && plan.scissors.empty()) {
        // A forced paint with no plan (first exposure, post-recovery) must still cover the window;
        // the plan-driven overload would return without painting because it has no regions.
        return state.pipeline->paint_frame(state.root, *state.provider, sink, true);
    }
    return state.pipeline->paint_frame(state.root, *state.provider, sink, plan, force);
}

void AppHost::end_frame() {
    impl_->dirty.clear(impl_->root, ui::Dirty::All);
}

void AppHost::observe_frame_time_ms(double frame_time_ms) {
    observe_frame_time_sample(frame_time_ms, std::nullopt);
}

void AppHost::observe_frame_time_sample(double cpu_frame_time_ms,
                                        std::optional<double> gpu_frame_time_ms) {
    // The stage owns the selection rule, the recording and both application rules (see
    // quality_stage.hpp); the host only hands it one observed frame.
    (void)impl_->quality.observe(cpu_frame_time_ms, gpu_frame_time_ms);
}

const runtime::AdaptiveQualitySample& AppHost::adaptive_quality() const {
    return impl_->quality.decision();
}

const runtime::FrameDiagnostics& AppHost::frame_diagnostics() const {
    return impl_->frame_diagnostics;
}

const ui::paint::PaintCacheStats& AppHost::paint_cache_stats() const {
    return impl_->pipeline->paint_cache_stats();
}

// The whole size change lives in host_viewport.cpp: the media context is part of it now (issue
// #436), and the rule that separates "re-resolve the cascade" from "re-lay out" is worth reading in
// one place rather than at the bottom of this file.
void AppHost::resize(float width, float height, float dpi_scale) {
    checked_extent(width, "width");
    checked_extent(height, "height");
    checked_scale(dpi_scale);
    impl_->apply_size(width, height, dpi_scale);
}

void AppHost::set_window_metrics(const hal::WindowMetrics& metrics) {
    Impl& state = *impl_;
    // A snapshot that names a usable extent is a size change too (issue #436), and it goes through
    // the same path `resize()` does: the media context is the window, so a driver that only feeds
    // metrics must not leave `@media` resolved against the previous rectangle. A snapshot without
    // one (a synthetic event that carries insets only) is not a zero-sized window, so it only moves
    // the safe-area snapshot.
    if (metrics.logical_width >= 1 && metrics.logical_height >= 1 && metrics.dpi_scale > 0.0f) {
        state.apply_size(static_cast<float>(metrics.logical_width),
                         static_cast<float>(metrics.logical_height), metrics.dpi_scale);
    }
    if (!state.safe_area.set_metrics(metrics, state.reduced_motion)) {
        return;
    }
    mark_safe_area_nodes(state.root);
    state.loop.mark_render_dirty();
}

ui::OverlayHost& AppHost::overlays() {
    if (!impl_->overlay_host) {
        impl_->overlay_host = std::make_unique<ui::OverlayHost>(impl_->root);
    }
    return *impl_->overlay_host;
}

ui::OverlayHandle AppHost::open_overlay(ui::OverlaySpec spec) {
    const ui::OverlayHandle handle = overlays().open(std::move(spec));
    impl_->loop.mark_render_dirty();
    return handle;
}

bool AppHost::close_overlay(ui::OverlayHandle handle) {
    const bool closed = overlays().close(handle);
    if (closed) {
        impl_->loop.mark_render_dirty();
    }
    return closed;
}

void AppHost::request_full_repaint() {
    Impl& state = *impl_;
    state.full_repaint_pending = true;
    // The frame may be on-demand and the scene clean: the repaint request is its own wake source.
    state.loop.mark_render_dirty();
}

void AppHost::register_embedded_stylesheets(std::span<const ui::style::EmbeddedStylesheet> sheets) {
    impl_->register_embedded_stylesheets(sheets);
}

std::size_t AppHost::rejected_stylesheet_count() const {
    return impl_->rejected_stylesheet_count;
}

void AppHost::set_update_scheduler(ui::UpdateScheduler* scheduler) {
    impl_->scheduler = scheduler;
    impl_->pipeline->set_update_scheduler(scheduler);
    // The settle loop's app callbacks are flushed through the same scheduler, so a component that
    // sets state from `onSizeChange` applies in the round that delivered it (issue #693).
    impl_->settle.set_scheduler(scheduler);
    if (scheduler != nullptr && impl_->reduced_motion) {
        scheduler->set_reduced_motion(true);
    }
}

void AppHost::set_reduced_motion(bool reduced) {
    impl_->reduced_motion = reduced;
    impl_->animations.set_reduced_motion(reduced);
    if (impl_->scheduler != nullptr) {
        impl_->scheduler->set_reduced_motion(reduced);
    }
}

bool AppHost::reduced_motion() const {
    return impl_->reduced_motion;
}

// `AppHost::set_appearance` (issue #187) lives in host_appearance.cpp: the rule it encodes - which
// appearance changes force a cascade re-resolve - is worth stating in one place, and host.cpp is at
// its size budget.

const ui::style::MediaContext& AppHost::media_context() const {
    return impl_->media_context;
}

std::uint64_t AppHost::cascade_generation() const {
    return impl_->cascade_generation;
}

bool AppHost::frame_due() const {
    return impl_->pipeline->frame_due();
}

ui::layout::LayoutObserverSet& AppHost::layout_observers() {
    return impl_->settle.observers();
}

const ui::layout::SettleTotals& AppHost::layout_settle_totals() const {
    return impl_->settle.totals();
}

const ui::frame::FrameRender& AppHost::last_render() const {
    return impl_->last_render;
}

void AppHost::paint_scene(ui::paint::PaintSink& sink, ui::paint::PaintContext context) {
    const ui::frame::InterpolationPaintView animated = impl_->pipeline->interpolation_view();
    if (!animated.empty()) {
        context.view = &animated;
    }
    ui::paint::paint_scene(impl_->root, *impl_->provider, sink, context);
}

ui::paint::StyleProvider& AppHost::styles() {
    return *impl_->provider;
}

ui::layout::LayoutEngine& AppHost::layout_engine() {
    return impl_->engine;
}

std::size_t AppHost::layout_runs() const {
    return impl_->layout_scheduler.layout_runs();
}

std::size_t AppHost::layout_cache_hits() const {
    return impl_->layout_scheduler.cache_hits();
}

std::size_t AppHost::layout_gate_hits() const {
    return impl_->layout_gate_hits;
}

const char* AppHost::layout_gate_reason() const {
    switch (impl_->layout_gate) {
    case Impl::LayoutGate::None:
        return "None";
    case Impl::LayoutGate::NoPreviousLayout:
        return "NoPreviousLayout";
    case Impl::LayoutGate::LayoutDirt:
        return "LayoutDirt";
    case Impl::LayoutGate::ShapeChanged:
        return "ShapeChanged";
    case Impl::LayoutGate::ViewportChanged:
        return "ViewportChanged";
    case Impl::LayoutGate::SafeAreaChanged:
        return "SafeAreaChanged";
    case Impl::LayoutGate::StyleMoved:
        return "StyleMoved";
    case Impl::LayoutGate::NotSubtreeLocal:
        return "NotSubtreeLocal";
    case Impl::LayoutGate::TreeRebuilt:
        return "TreeRebuilt";
    }
    return "?";
}

ui::animation::AnimationRuntime& AppHost::animations() {
    return impl_->animations;
}

ui::animation::SpringRuntime& AppHost::springs() {
    return impl_->pipeline->springs();
}

float AppHost::width() const {
    return impl_->config.width;
}

float AppHost::height() const {
    return impl_->config.height;
}

float AppHost::dpi_scale() const {
    return impl_->config.dpi_scale;
}

rendering::rhi::Extent2D AppHost::target_extent() const {
    return impl_->damage->target();
}

} // namespace tessera::app
