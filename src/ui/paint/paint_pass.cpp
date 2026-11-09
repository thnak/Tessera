// SPDX-License-Identifier: MIT

#include "paint_properties.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace tessera::ui::paint {
namespace {

// Bounds are parent-relative; Node::world_bounds() composes them into absolute coordinates, so the
// pass paints that rect directly (see paint_pass.hpp).
rhi::Rect paint_rect(const Node& node) {
    return node.world_bounds();
}

float clamp_radius(float radius, const rhi::Rect& rect) {
    return std::min({radius, rect.width * 0.5f, rect.height * 0.5f});
}

void emit_rect(PaintSink& sink, const rhi::Rect& rect, const std::optional<rhi::Color>& color,
               float opacity, std::size_t& counter) {
    if (!color || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    rhi::Color value = *color;
    value.a *= opacity;
    if (value.a <= 0.0f) {
        return;
    }
    sink.fill_rect(rect, value);
    ++counter;
}

void paint_background(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                      const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    const std::optional<rhi::Color> color = detail::color_value(style, "background-color", current);
    if (!color || color->a <= 0.0f || opacity <= 0.0f) {
        return;
    }
    rhi::Color value = *color;
    value.a *= opacity;
    if (value.a <= 0.0f) {
        return;
    }

    const std::optional<float> radius = detail::uniform_radius(style);
    if (radius && *radius > 0.0f) {
        sink.fill_rounded_rect(rect, value, clamp_radius(*radius, rect));
    } else {
        sink.fill_rect(rect, value);
    }
    ++stats.backgrounds;
}

void paint_borders(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                   const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    float top = detail::border_width(style, "border-top-width", "border-top-style");
    float right = detail::border_width(style, "border-right-width", "border-right-style");
    float bottom = detail::border_width(style, "border-bottom-width", "border-bottom-style");
    float left = detail::border_width(style, "border-left-width", "border-left-style");

    // Keep opposing edges from overlapping so every strip stays a disjoint rect.
    top = std::min(top, rect.height);
    bottom = std::min(bottom, rect.height - top);
    left = std::min(left, rect.width);
    right = std::min(right, rect.width - left);

    const float middle_height = rect.height - top - bottom;

    emit_rect(sink, rhi::Rect{rect.x, rect.y, rect.width, top},
              detail::color_value(style, "border-top-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x + rect.width - right, rect.y + top, right, middle_height},
              detail::color_value(style, "border-right-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + rect.height - bottom, rect.width, bottom},
              detail::color_value(style, "border-bottom-color", current), opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + top, left, middle_height},
              detail::color_value(style, "border-left-color", current), opacity, stats.borders);
}

// Paints an optional PaintData text run (D9) after the node's background/borders and inside the
// active clip. Precedence: PaintData override > computed style > backend default; node opacity
// multiplies the effective color alpha.
void paint_text(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                PaintSink& sink, PaintStats& stats) {
    const auto& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text || data->text.empty()) {
        return;
    }
    if (rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    const float opacity = detail::opacity_value(style);
    if (opacity <= 0.0f) {
        return;
    }

    TextDraw run;
    run.rect = rect;
    run.text = data->text;
    run.font_size = data->text_style.font_size > 0.0f ? data->text_style.font_size
                                                      : detail::font_size_value(style);
    run.font_weight = data->text_style.font_weight > 0 ? data->text_style.font_weight
                                                       : detail::font_weight_value(style);
    run.color = data->text_style.color.value_or(detail::current_color(style));
    run.color.a *= opacity;
    if (run.font_size <= 0.0f || run.color.a <= 0.0f) {
        return;
    }
    sink.draw_text(run);
    ++stats.texts;
}

void paint_node(const Node& node, StyleProvider& styles, PaintSink& sink, PaintStats& stats) {
    if (!node.visible()) {
        ++stats.hidden_subtrees;
        return;
    }

    const style::ComputedStyle style = styles.compute(node);
    ++stats.nodes;

    const rhi::Rect rect = paint_rect(node);
    if (rect.width > 0.0f && rect.height > 0.0f) {
        const float opacity = detail::opacity_value(style);
        const rhi::Color current = detail::current_color(style);
        paint_background(style, rect, opacity, current, sink, stats);
        paint_borders(style, rect, opacity, current, sink, stats);
    }

    // `overflow` clipping applies to the children (the element's own background/border are painted
    // above). Scroll containers clip their children as well: the children are offset by -scroll via
    // world_transform(), and the clip keeps them inside the container box. The clip is the border
    // box because the paint model has no padding-box yet.
    const bool scroll_clips = node.scrollable_x() || node.scrollable_y();
    const bool clips =
        (detail::clips_children(style) || scroll_clips) && rect.width > 0.0f && rect.height > 0.0f;
    if (clips) {
        sink.push_clip(detail::corner_clip(style, rect));
        ++stats.clips;
    }
    // Text runs are content: paint them after the element's own background/borders and inside its
    // own overflow clip, before the children (paint order).
    paint_text(node, style, rect, sink, stats);
    for (const auto& child : node.children()) {
        paint_node(*child, styles, sink, stats);
    }
    if (clips) {
        sink.pop_clip();
    }
}

} // namespace

SceneStyleProvider::SceneStyleProvider(const style::CompiledStyleSheet& sheet,
                                       style::SceneStyleTree& tree)
    : sheet_(&sheet),
      tree_(&tree) {}

style::ComputedStyle SceneStyleProvider::compute(const Node& node) {
    return tree_->compute(*sheet_, node);
}

PaintStats paint_scene(const Node& root, StyleProvider& styles, PaintSink& sink) {
    PaintStats stats;
    paint_node(root, styles, sink, stats);
    return stats;
}

} // namespace tessera::ui::paint
