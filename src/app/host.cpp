// SPDX-License-Identifier: MIT

#include "host_impl.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/frame_time_source.hpp"
#include "tessera/ui/scene/paint_data.hpp"

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

namespace rhi = tessera::rendering::rhi;

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
    if (is_safe_area_node(node)) {
        node.mark_dirty(ui::Dirty::Layout);
    }
    for (const auto& child : node.children()) {
        mark_safe_area_nodes(*child);
    }
}

void mark_subtree(ui::Node& node, ui::Dirty dirty) {
    node.mark_dirty(dirty);
    for (const auto& child : node.children()) {
        mark_subtree(*child, dirty);
    }
}

// One layout tree of computed styles (the paint pass resolves the same provider, so both agree).
ui::layout::LayoutInput build_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles,
                                           const SafeAreaState& safe_area) {
    ui::layout::LayoutInput input;
    input.id = node.id();
    input.style = styles.compute(node);
    // A SafeArea node's padding or margin is host-owned: the resolved insets replace the sides.
    if (safe_area.has_metrics()) {
        const std::string* direction = input.style.find("direction");
        if (const auto spec =
                read_safe_area_spec(node, direction != nullptr && *direction == "rtl")) {
            apply_safe_area(input.style, *spec, safe_area.resolve(spec->config));
        }
    }
    const auto& paint = node.paint_data();
    const bool text_payload = paint && paint->kind == ui::PaintKind::Text;
    if (text_payload) {
        input.text = paint->text;
    }
    for (const auto& child : node.children()) {
        // Inline spans are runs of this text, not boxes (issue #229, elements-basic.md 2.1).
        if (text_payload && paint->has_span(child->id())) {
            continue;
        }
        input.children.push_back(build_layout_input(*child, styles, safe_area));
    }
    return input;
}

bool has_inline_style(const ui::Node& node, std::string_view name) {
    for (const ui::Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        if (declaration.name == name) {
            return true;
        }
    }
    return false;
}

rhi::Extent2D extent_for(float width, float height, float dpi_scale) {
    const auto physical = [](float logical, float scale) {
        const float scaled = std::ceil(logical * scale);
        return static_cast<std::uint32_t>(std::clamp(scaled, 1.0f, kMaxExtent));
    };
    return rhi::Extent2D{physical(width, dpi_scale), physical(height, dpi_scale)};
}

const char* blob_error_name(ui::style::BlobErrorCode code) {
    switch (code) {
    case ui::style::BlobErrorCode::Truncated:
        return "Truncated";
    case ui::style::BlobErrorCode::BadMagic:
        return "BadMagic";
    case ui::style::BlobErrorCode::UnsupportedFormatVersion:
        return "UnsupportedFormatVersion";
    case ui::style::BlobErrorCode::EngineSchemaMismatch:
        return "EngineSchemaMismatch";
    case ui::style::BlobErrorCode::HashMismatch:
        return "HashMismatch";
    case ui::style::BlobErrorCode::Corrupt:
        return "Corrupt";
    }
    return "Corrupt";
}

// A skipped embedded sheet is reported, never silently swallowed (ADR-048: an embedded blob is
// build output, so a load failure is a build/runtime version mismatch, not a CSS error).
void report_blob_failure(std::string_view path, const ui::style::BlobError& error) {
    std::fprintf(stderr, "TESSERA_CSS_BLOB_INVALID: %.*s: %s: %s\n", static_cast<int>(path.size()),
                 path.data(), blob_error_name(error.code), error.message.c_str());
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

// True when some visible node paints a blurred outer shadow: the effect the first ladder position
// reduces. Read from the outsets the style sync already stores on each node, so it needs no style
// resolution. Allocation-free; it runs only on the observation that changes the ladder position.
bool scene_has_blurred_shadow(const ui::Node& node) noexcept {
    if (!node.rendered()) {
        return false;
    }
    if (node.paint_outsets().blurred) {
        return true;
    }
    for (const auto& child : node.children()) {
        if (scene_has_blurred_shadow(*child)) {
            return true;
        }
    }
    return false;
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
      damage(std::make_unique<rendering::DamageAccumulator>(rendering::DamageConfig{
          extent_for(config.width, config.height, config.dpi_scale), config.damage_max_rects})),
      quality_controller(quality_controller_config(config.adaptive_quality)) {
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
    frame_diagnostics.record_adaptive_quality(quality_controller.decision());
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
}

void AppHost::Impl::rebuild_style_tree() {
    // Attaching/removing children invalidates the captured tree (SceneStyleTree), and the
    // provider borrows it, so both are recreated together.
    tree = std::make_unique<ui::style::SceneStyleTree>(root);
    provider = std::make_unique<ui::paint::SceneStyleProvider>(sheet, *tree);
}

// One cascade: the constructor sheet (lowest source order), the embedded sheets in table order,
// and the active theme's generated `:root` rule last with `Origin::Theme` (stylesheet-v1.md
// section 9.1). The theme rule cannot win an author declaration of the same token because the
// theme origin ranks below author, and only the active theme is present.
void AppHost::Impl::rebuild_cascade() {
    ui::style::CompiledStyleSheet combined = base_sheet;
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
    mark_subtree(root, dirty_mask);
    loop.mark_render_dirty();
}

void AppHost::Impl::run_layout_pass() {
    const ui::layout::LayoutInput input = build_layout_input(root, *provider, safe_area);
    const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
    // The scheduler reuses the cached result when the layout input signature (tree shape,
    // layout-affecting longhands, text, viewport) is unchanged, so a style-only change that
    // cannot move boxes skips Yoga and only re-syncs the scene style.
    last_layout = layout_scheduler.update(input, viewport, true);
    last_applied = ui::layout::apply_layout(last_layout, root);
}

void AppHost::Impl::register_embedded_stylesheets(
    std::span<const ui::style::EmbeddedStylesheet> sheets) {
    // Registration is a startup-time operation: every blob is loaded here, once, and resolved
    // into the cascade; no frame ever parses CSS. The constructor sheet is kept as the base of
    // every rebuild, so a later call replaces the previous registration rather than stacking.
    embedded_sheets.clear();
    rejected_stylesheet_count = 0;
    for (const ui::style::EmbeddedStylesheet& embedded : sheets) {
        auto prepared = ui::style::load_stylesheet_blob(embedded.bytes);
        if (!prepared) {
            report_blob_failure(embedded.path, prepared.error());
            ++rejected_stylesheet_count;
            continue;
        }
        embedded_sheets.push_back(std::move(*prepared));
    }
    // The provider borrows the active sheet and the tree captures the scene, so both are rebuilt
    // now (a direct paint before the next begin_frame() stays consistent). The style generation
    // changed: mark every node so the next begin_frame() re-resolves and rebuilds the selector
    // tree, and drop the per-node paint cache because a node can stay Clean while its computed
    // style changes (inheritance, ancestor/sibling selectors, a new sheet).
    apply_style_generation(ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint |
                           ui::Dirty::Render);
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
    impl_->animations.set_reduced_motion(config.reduced_motion);
}

AppHost::~AppHost() = default;

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
    // selector tree and re-run layout for both; a paint-only frame skips all of it.
    const bool geometry_work = frame.tick.needs_style || frame.tick.needs_layout;
    if (geometry_work) {
        state.rebuild_style_tree();
        // A rebuilt selector tree is a new style generation. The paint cache key holds a node's own
        // dirty flags, not its computed style, so a node that stayed Clean can still resolve a
        // different style now (an inherited value, an ancestor/sibling selector, a restyled root or
        // a new theme). Dropping every record is the only sound answer; replaying one would paint
        // the previous generation and differ from a cold full paint.
        state.pipeline->invalidate_paint_cache();
        frame.style_rebuilt = true;
        state.run_layout_pass();
        frame.layout = state.last_applied;
        frame.sync = state.pipeline->sync_style(state.root, *state.provider, frame.tick, true);
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
    Impl& state = *impl_;
    // The pure rule (frame_time_source.hpp) picks the signal; the capability decision was already
    // made by the device probe, so the worst case here is a CPU fallback, never a guessed GPU
    // number. The controller returns the decision in force with this frame's change flags;
    // recording it always keeps the latest scale readable and only counts the frames that changed
    // something.
    const runtime::FrameTimeSample sample =
        runtime::select_frame_time(cpu_frame_time_ms, gpu_frame_time_ms);
    state.frame_diagnostics.record_frame_time_source(sample.source);
    // The probe is read only when the controller is about to change position
    // (adaptive_quality.hpp), so the scene walk below costs nothing on an ordinary frame.
    const runtime::AdaptiveQualityStageProbe probe{
        [](const void* context) noexcept {
            return scene_has_blurred_shadow(static_cast<const Impl*>(context)->root);
        },
        [](const void* context) noexcept {
            return static_cast<const Impl*>(context)->animations.non_essential_active() > 0;
        },
        &state};
    const runtime::AdaptiveQualitySample decision =
        state.quality_controller.observe(sample.frame_time_ms, probe);
    state.frame_diagnostics.record_adaptive_quality(decision);
    if (decision.effects_reduced != state.pipeline->reduce_effects()) {
        state.pipeline->set_reduce_effects(decision.effects_reduced);
        // Pixels change with the flag, and a settled on-demand scene has nothing else to repaint.
        request_full_repaint();
    }
    if (decision.animations_paused != state.animations.suspend_non_essential()) {
        state.animations.set_suspend_non_essential(decision.animations_paused);
        // Resuming must wake a loop that went idle while the animations were frozen.
        state.loop.mark_render_dirty();
    }
}

const runtime::AdaptiveQualitySample& AppHost::adaptive_quality() const {
    return impl_->quality_controller.decision();
}

const runtime::FrameDiagnostics& AppHost::frame_diagnostics() const {
    return impl_->frame_diagnostics;
}

const ui::paint::PaintCacheStats& AppHost::paint_cache_stats() const {
    return impl_->pipeline->paint_cache_stats();
}

void AppHost::resize(float width, float height, float dpi_scale) {
    Impl& state = *impl_;
    checked_extent(width, "width");
    checked_extent(height, "height");
    checked_scale(dpi_scale);
    if (width == state.config.width && height == state.config.height &&
        dpi_scale == state.config.dpi_scale) {
        return;
    }
    state.config.width = width;
    state.config.height = height;
    state.config.dpi_scale = dpi_scale;
    state.apply_root_size();

    // The damage target and the pipeline borrow the accumulator, so both are recreated. Old damage
    // rectangles belong to the previous size; a full redraw is the only safe plan.
    state.tracker.clear();
    state.damage = std::make_unique<rendering::DamageAccumulator>(rendering::DamageConfig{
        extent_for(width, height, dpi_scale), state.config.damage_max_rects});
    state.damage->force_full_redraw();
    state.pipeline = std::make_unique<ui::frame::FramePipeline>(state.animations, state.loop,
                                                                state.tracker, *state.damage,
                                                                state.dirty, state.config.pipeline);
    state.pipeline->set_update_scheduler(state.scheduler);
    // The new pipeline starts at full quality; carry the controller's decision over.
    state.pipeline->set_reduce_effects(state.quality_controller.decision().effects_reduced);
    mark_subtree(state.root, ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render);
}

void AppHost::set_window_metrics(const hal::WindowMetrics& metrics) {
    Impl& state = *impl_;
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

bool AppHost::frame_due() const {
    return impl_->pipeline->frame_due();
}

const ui::frame::FrameRender& AppHost::last_render() const {
    return impl_->last_render;
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
