// SPDX-License-Identifier: MIT
//
// The viewport half of `AppHost` (issue #436): a window or emulator resize must reach the
// stylesheet and the tree, not only the boxes.
//
// **The bug this file exists to fix.** `AppHost::resize` updated the root size, recreated the
// damage tracker and marked layout dirty, but never touched `media_context`. That context defaults
// to 1280x720 and only `set_appearance` ever moved it, so `@media (max-width: 600px)`,
// `(orientation: portrait)` and the viewport units were resolved against a rectangle no window had:
// the boxes re-laid out and the responsive stylesheet did not respond.
//
// **The rule.** The window IS the media context: `viewport_width/height` are the logical client
// area and `resolution_dpi` is the DPI scale in CSS dpi (96 dpi at scale 1). Both entry points that
// can move it land in `apply_size()` - `AppHost::resize` (the driver's resize) and
// `set_window_metrics` (a platform snapshot that names a different extent) - so there is one answer
// to "what does a new window rectangle do".
//
// **And no re-resolve unless a media answer can change.** A drag-resize arrives as many size
// changes per second, and re-resolving the cascade on each of them would throw away the paint cache
// for nothing: a cascade rebuild is only owed when a `@media` answer actually flips.
// `apply_viewport()` asks the prepared sheet that question
// (`PreparedStyleSheet::media_answer_can_change`, which compares the very booleans the resolve step
// evaluates) and otherwise does what a resize always did - one layout pass. The generation counter
// `AppHost::cascade_generation()` is how a test, or a reviewer watching the browser, sees the
// difference.
//
// Split out of `host.cpp`, which is over the size budget.

#include "host_impl.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/window_environment.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tessera::app {

namespace style = tessera::ui::style;

namespace {

// CSS defines the reference density; a `dpi_scale` is a multiple of it, so `(min-resolution:
// 2dppx)` is 192 dpi on a 2x display (media_query.cpp compares `MediaContext::resolution_dpi`
// against the query's resolution, `dppx * 96`).
constexpr double kCssDpi = 96.0;

constexpr float kMaxExtent = 65535.0f;

} // namespace

namespace detail {

rendering::rhi::Extent2D extent_for(float width, float height, float dpi_scale) {
    // The logical size times the scale, rounded up and clamped: the damage target is a whole number
    // of device pixels and can never be zero (a zero-sized target is not a frame).
    const auto physical = [](float logical, float scale) {
        const float scaled = std::ceil(logical * scale);
        return static_cast<std::uint32_t>(std::clamp(scaled, 1.0f, kMaxExtent));
    };
    return rendering::rhi::Extent2D{physical(width, dpi_scale), physical(height, dpi_scale)};
}

void mark_subtree(ui::Node& node, ui::Dirty dirty) {
    node.mark_dirty(dirty);
    for (const auto& child : node.children()) {
        mark_subtree(*child, dirty);
    }
}

} // namespace detail

style::MediaContext window_media_context(float width, float height, float dpi_scale,
                                         const hal::AppearancePreference& appearance) noexcept {
    style::MediaContext context;
    context.viewport_width = static_cast<double>(width);
    context.viewport_height = static_cast<double>(height);
    context.resolution_dpi = kCssDpi * static_cast<double>(dpi_scale);
    // The OS preferences are not a property of the window rectangle, so they are folded in here
    // rather than by the caller: one context, one construction rule (`with_appearance` is the one
    // mapping from the HAL snapshot to CSS).
    return style::with_appearance(context, appearance);
}

// A new window rectangle: root size, damage target, pipeline and - the part issue #436 adds - the
// media context, with the cascade re-resolved only when a media answer can move.
void AppHost::Impl::apply_size(float width, float height, float dpi_scale) {
    if (width == config.width && height == config.height && dpi_scale == config.dpi_scale) {
        return;
    }
    config.width = width;
    config.height = height;
    config.dpi_scale = dpi_scale;
    apply_root_size();

    // The damage target and the pipeline borrow the accumulator, so both are recreated. Old damage
    // rectangles belong to the previous size; a full redraw is the only safe plan.
    tracker.clear();
    damage = std::make_unique<rendering::DamageAccumulator>(rendering::DamageConfig{
        detail::extent_for(width, height, dpi_scale), config.damage_max_rects});
    damage->force_full_redraw();
    pipeline = std::make_unique<ui::frame::FramePipeline>(animations, loop, tracker, *damage, dirty,
                                                          config.pipeline);
    pipeline->set_update_scheduler(scheduler);
    // The new pipeline starts at full quality; carry the controller's decision over.
    pipeline->set_reduce_effects(quality.decision().effects_reduced);

    apply_viewport(ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render);
}

// The media half of a size change (issue #436). The three viewport fields are what a window
// rectangle owns; the appearance fields are already in `media_context` and are kept.
void AppHost::Impl::apply_viewport(ui::Dirty dirty_mask) {
    const style::MediaContext next = style::with_fold(
        window_media_context(config.width, config.height, config.dpi_scale, appearance),
        window_fold);
    if (next.viewport_width == media_context.viewport_width &&
        next.viewport_height == media_context.viewport_height &&
        next.resolution_dpi == media_context.resolution_dpi) {
        return; // the rectangle did not move, so no `@media` answer can have
    }
    const bool cascade_moves = media_answer_can_change(media_context, next);
    media_context = next;
    // `useBreakpoint()` reads the same context the `@media` rules do, so the class is pushed here,
    // before the style pass that will paint it (issue #692). A resize inside one class returns
    // immediately, so it costs no walk of the consumer set.
    push_size_class();
    if (!cascade_moves) {
        // No breakpoint was crossed: every rule that applied still applies, so the boxes re-lay out
        // against the new viewport and the cascade is left alone (a layout pass, not a re-resolve).
        detail::mark_subtree(root, dirty_mask);
        loop.mark_render_dirty();
        return;
    }
    // A media answer moved, so the cascade is stale: re-resolve the SAME prepared data (ADR-048's
    // resolve step, never a re-parse), rebuild the selector tree, drop the paint cache and mark
    // every node. `apply_style_generation` also marks Style, which a re-resolve needs and a plain
    // resize does not.
    apply_style_generation(dirty_mask | ui::Dirty::Style);
}

// A new fold (ADR-074). The hinge rectangle and the angle are not media answers, so a snapshot that
// moves only those leaves the context equal and returns before asking the sheet anything.
void AppHost::Impl::apply_fold(const hal::FoldInfo& fold) {
    window_fold = fold;
    const style::MediaContext next = style::with_fold(media_context, fold);
    if (next.device_posture == media_context.device_posture &&
        next.horizontal_viewport_segments == media_context.horizontal_viewport_segments &&
        next.vertical_viewport_segments == media_context.vertical_viewport_segments) {
        return;
    }
    const bool cascade_moves = media_answer_can_change(media_context, next);
    media_context = next;
    if (cascade_moves) {
        apply_style_generation(ui::Dirty::Style | ui::Dirty::Paint | ui::Dirty::Render);
    }
}

// New `env()` values (ADR-074). The size is the host's own, because a snapshot that carries only
// insets has no extent of its own.
void AppHost::Impl::apply_environment(const hal::WindowMetrics& metrics) {
    style::StyleEnvironment next = style::window_environment(metrics, config.width, config.height);
    static const style::StyleEnvironment kNone;
    const style::StyleEnvironment& before =
        style_environment != nullptr ? *style_environment : kNone;
    if (next == before) {
        return;
    }
    const bool cascade_moves = environment_answer_can_change(before, next);
    // A hinge that moved is the only change a sheet can only paint with (ADR-074 section 4), so it
    // restyles without a layout pass; anything else can feed a box.
    const bool paint_only = style::differs_only_in_continuous(before, next);
    style_environment = std::make_shared<const style::StyleEnvironment>(std::move(next));
    sheet.set_environment(style_environment);
    if (cascade_moves) {
        // A discrete variable can feed any property, layout included.
        apply_style_generation(paint_only ? ui::Dirty::Style | ui::Dirty::Paint | ui::Dirty::Render
                                          : ui::Dirty::Style | ui::Dirty::Layout |
                                                ui::Dirty::Paint | ui::Dirty::Render);
    }
}

bool AppHost::Impl::environment_answer_can_change(const style::StyleEnvironment& before,
                                                  const style::StyleEnvironment& after) const {
    if (base_prepared.has_value() && base_prepared->environment_answer_can_change(before, after)) {
        return true;
    }
    return std::any_of(embedded_sheets.begin(), embedded_sheets.end(),
                       [&](const style::PreparedStyleSheet& prepared) {
                           return prepared.environment_answer_can_change(before, after);
                       });
}

// True when resolving the sheet against `before` and against `after` can yield a different cascade.
// Only the prepared (media-independent) form can answer that, which is why a host built through the
// already-compiled `CompiledStyleSheet` constructor cannot follow a media change - the same
// property, and the same caveat, as `set_appearance`.
bool AppHost::Impl::media_answer_can_change(const style::MediaContext& before,
                                            const style::MediaContext& after) const {
    if (base_prepared.has_value() && base_prepared->media_answer_can_change(before, after)) {
        return true;
    }
    for (const style::PreparedStyleSheet& prepared : embedded_sheets) {
        if (prepared.media_answer_can_change(before, after)) {
            return true;
        }
    }
    // The theme sheet is generated rather than parsed and carries one `:root` rule with no
    // `@media`, so it cannot answer differently for two viewports (`prepare_theme_stylesheet`).
    return false;
}

} // namespace tessera::app
