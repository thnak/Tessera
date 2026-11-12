// SPDX-License-Identifier: MIT

#include "tessera/app/host.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
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

void mark_subtree(ui::Node& node, ui::Dirty dirty) {
    node.mark_dirty(dirty);
    for (const auto& child : node.children()) {
        mark_subtree(*child, dirty);
    }
}

// The layout engine consumes a plain tree of computed styles; the paint pass resolves the same
// styles again through the provider, so the two always agree on one cascade.
ui::layout::LayoutInput build_layout_input(const ui::Node& node, ui::paint::StyleProvider& styles) {
    ui::layout::LayoutInput input;
    input.id = node.id();
    input.style = styles.compute(node);
    const auto& paint = node.paint_data();
    if (paint && paint->kind == ui::PaintKind::Text) {
        input.text = paint->text;
    }
    for (const auto& child : node.children()) {
        input.children.push_back(build_layout_input(*child, styles));
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

} // namespace

struct AppHost::Impl {
    Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
         HostConfig config)
        : root(root),
          sheet(std::move(sheet)),
          loop(loop),
          config(config),
          engine(config.text_measurer),
          damage(std::make_unique<rendering::DamageAccumulator>(
              rendering::DamageConfig{extent_for(config.width, config.height, config.dpi_scale),
                                      config.damage_max_rects})) {
        // The window root is the host's geometry: logical size plus the column flex container every
        // app content area starts from. Inline style is the outermost origin, so an app stylesheet
        // can style the root's background but never resize the window.
        apply_root_size();
        tree = std::make_unique<ui::style::SceneStyleTree>(root);
        provider = std::make_unique<ui::paint::SceneStyleProvider>(this->sheet, *tree);
        pipeline = std::make_unique<ui::frame::FramePipeline>(
            animations, this->loop, tracker, *damage, dirty, this->config.pipeline);
    }

    void apply_root_size() {
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

    void rebuild_style_tree() {
        // Attaching/removing children invalidates the captured tree (SceneStyleTree), and the
        // provider borrows it, so both are recreated together.
        tree = std::make_unique<ui::style::SceneStyleTree>(root);
        provider = std::make_unique<ui::paint::SceneStyleProvider>(sheet, *tree);
    }

    void run_layout_pass() {
        const ui::layout::LayoutInput input = build_layout_input(root, *provider);
        const ui::layout::LayoutViewport viewport{config.width, config.height, config.dpi_scale};
        // The scheduler reuses the cached result when the layout input signature (tree shape,
        // layout-affecting longhands, text, viewport) is unchanged, so a style-only change that
        // cannot move boxes skips Yoga and only re-syncs the scene style.
        last_layout = layout_scheduler.update(input, viewport, true);
        last_applied = ui::layout::apply_layout(last_layout, root);
    }

    ui::Node& root;
    ui::style::CompiledStyleSheet sheet;
    runtime::RuntimeLoop& loop;
    HostConfig config;

    // Host-owned storage for the root's inline `width`/`height` text (the scene borrows it).
    std::array<char, 32> root_width_text_{};
    std::array<char, 32> root_height_text_{};

    std::unique_ptr<ui::style::SceneStyleTree> tree;
    std::unique_ptr<ui::paint::SceneStyleProvider> provider;
    ui::layout::LayoutEngine engine;
    ui::layout::LayoutScheduler layout_scheduler{engine};
    ui::DirtyPass dirty;
    ui::layout::DamageTracker tracker;
    std::unique_ptr<rendering::DamageAccumulator> damage;
    ui::animation::AnimationRuntime animations;
    std::unique_ptr<ui::frame::FramePipeline> pipeline;
    ui::UpdateScheduler* scheduler = nullptr;
    bool reduced_motion = false;

    ui::layout::LayoutResult last_layout;
    ui::layout::LayoutApplyResult last_applied;
    ui::frame::FrameRender last_render;
};

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
    frame.tick = state.pipeline->tick(state.root, dt, alpha);

    // Any style or layout work may have changed the tree's shape (a state change can attach a
    // conditional child), and the damage pass latches a full redraw on overflow. Rebuild the
    // selector tree and re-run layout for both; a paint-only frame skips all of it.
    const bool geometry_work = frame.tick.needs_style || frame.tick.needs_layout;
    if (geometry_work) {
        state.rebuild_style_tree();
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
    mark_subtree(state.root, ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render);
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
