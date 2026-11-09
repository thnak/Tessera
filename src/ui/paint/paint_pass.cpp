// SPDX-License-Identifier: MIT

#include "paint_properties.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

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

struct BorderWidths {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};

// The four painted border widths, clamped against each other so the edge strips stay disjoint.
BorderWidths border_widths(const style::ComputedStyle& style, const rhi::Rect& rect) {
    BorderWidths widths;
    widths.top = detail::border_width(style, "border-top-width", "border-top-style");
    widths.right = detail::border_width(style, "border-right-width", "border-right-style");
    widths.bottom = detail::border_width(style, "border-bottom-width", "border-bottom-style");
    widths.left = detail::border_width(style, "border-left-width", "border-left-style");

    widths.top = std::min(widths.top, rect.height);
    widths.bottom = std::min(widths.bottom, rect.height - widths.top);
    widths.left = std::min(widths.left, rect.width);
    widths.right = std::min(widths.right, rect.width - widths.left);
    return widths;
}

// The padding box: the border box inset by the painted border widths. CSS clips a container's
// children at this edge, so they cannot paint over its border. Empty when the insets consume the
// box (the caller then skips the clip instead of pushing a degenerate one).
std::optional<rhi::Rect> padding_box(const style::ComputedStyle& style, const rhi::Rect& rect) {
    const BorderWidths widths = border_widths(style, rect);
    const float width = rect.width - widths.left - widths.right;
    const float height = rect.height - widths.top - widths.bottom;
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return rhi::Rect{rect.x + widths.left, rect.y + widths.top, width, height};
}

void paint_borders(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                   const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    const BorderWidths widths = border_widths(style, rect);
    const float top = widths.top;
    const float right = widths.right;
    const float bottom = widths.bottom;
    const float left = widths.left;

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
// multiplies the effective color alpha. NoWrap (default) emits one run; Wrap uses the shared
// text-line-box layout (`text::layout_text`) with the content box width and emits one run per line.
PaintTextAlign computed_text_align(const style::ComputedStyle& style) {
    const std::string* value = style.find("text-align");
    if (value == nullptr) {
        return PaintTextAlign::Start;
    }
    std::string token(*value);
    std::size_t first = 0;
    while (first < token.size() && std::isspace(static_cast<unsigned char>(token[first])) != 0) {
        ++first;
    }
    std::size_t last = token.size();
    while (last > first && std::isspace(static_cast<unsigned char>(token[last - 1])) != 0) {
        --last;
    }
    token = token.substr(first, last - first);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (token == "center") {
        return PaintTextAlign::Center;
    }
    if (token == "end" || token == "right") {
        return PaintTextAlign::End;
    }
    return PaintTextAlign::Start;
}

void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
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

    text::TextStyle text_style = text::text_style_from(style);
    if (data->text_style.font_size > 0.0f) {
        text_style.font_size = data->text_style.font_size;
    }
    if (data->text_style.font_weight > 0) {
        text_style.font_weight = data->text_style.font_weight;
    }

    const float padding_left = detail::length_value(style, "padding-left").value_or(0.0f);
    const float padding_right = detail::length_value(style, "padding-right").value_or(0.0f);
    const float padding_top = detail::length_value(style, "padding-top").value_or(0.0f);
    const float padding_bottom = detail::length_value(style, "padding-bottom").value_or(0.0f);
    const float content_width = std::max(0.0f, rect.width - padding_left - padding_right);
    const float content_height = std::max(0.0f, rect.height - padding_top - padding_bottom);
    const float content_x = rect.x + padding_left;
    const float content_y = rect.y + padding_top;

    rhi::Color color = data->text_style.color.value_or(detail::current_color(style));
    color.a *= opacity;
    if (content_width <= 0.0f || content_height <= 0.0f || color.a <= 0.0f ||
        text_style.font_size <= 0.0f) {
        return;
    }

    // `wrap` is authoritative (NoWrap is the V1 default); computed `white-space: nowrap|pre` can
    // only suppress wrapping, never opt into it.
    const bool wrap = data->wrap == PaintWrap::Wrap && !text_style.nowrap;
    if (!wrap) {
        TextDraw run;
        run.rect = {content_x, content_y, content_width, content_height};
        run.text = data->text;
        run.font_size = text_style.font_size;
        run.font_weight = text_style.font_weight;
        run.color = color;
        sink.draw_text(run);
        ++stats.texts;
        return;
    }

    const text::FontMetrics metrics = text::StubFontMetricsProvider{}.metrics(text_style);
    text::TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = content_width;
    const text::TextLayout layout = text::layout_text(data->text, text_style, metrics, constraints);
    std::size_t line_count = layout.lines.size();
    if (data->max_lines > 0) {
        line_count = std::min(line_count, static_cast<std::size_t>(data->max_lines));
    }

    const PaintTextAlign align = data->text_align.value_or(computed_text_align(style));
    float line_top = content_y;
    for (std::size_t index = 0; index < line_count; ++index) {
        const text::TextLine& line = layout.lines[index];
        if (line.end > line.begin) {
            float line_x = content_x;
            if (align == PaintTextAlign::Center) {
                line_x += (content_width - line.width) * 0.5f;
            } else if (align == PaintTextAlign::End) {
                line_x += content_width - line.width;
            }
            line_x = std::max(content_x, line_x);

            TextDraw run;
            // Integer snapping happens at paint time only (layout widths/offsets stay fractional).
            run.rect = {std::round(line_x), std::round(line_top), line.width, line.height};
            run.text = std::string_view(data->text).substr(line.begin, line.end - line.begin);
            run.font_size = text_style.font_size;
            run.font_weight = text_style.font_weight;
            run.color = color;
            run.baseline = std::round(line_top + line.baseline);
            sink.draw_text(run);
            ++stats.texts;
        }
        line_top += line.height;
    }
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
    // world_transform(), and the clip keeps them inside the container's padding box - the border
    // box inset by the painted border widths, which is the CSS region children may not paint into.
    // The scrollport used for clamping is still the border box (see scroll-and-gestures-v1.md 2.1).
    const bool scroll_clips = node.scrollable_x() || node.scrollable_y();
    const bool clip_children =
        (detail::clips_children(style) || scroll_clips) && rect.width > 0.0f && rect.height > 0.0f;
    bool pushed_clip = false;
    if (clip_children) {
        if (const std::optional<rhi::Rect> clip_rect = padding_box(style, rect)) {
            sink.push_clip(detail::corner_clip(style, *clip_rect));
            ++stats.clips;
            pushed_clip = true;
        }
    }
    // Text runs are content: paint them after the element's own background/borders and inside its
    // own overflow/scroll clip, before the children (paint order).
    paint_text_lines(node, style, rect, sink, stats);
    for (const auto& child : node.children()) {
        paint_node(*child, styles, sink, stats);
    }
    if (pushed_clip) {
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
