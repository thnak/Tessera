// SPDX-License-Identifier: MIT

#include "paint_properties.hpp"
#include "paint_vector.hpp"
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

void paint_borders(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                   const rhi::Color& current, PaintSink& sink, PaintStats& stats) {
    const detail::BorderWidths widths = detail::border_widths(style, rect);
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

// Caret/selection emission. Paint only consumes byte offsets: it resolves them through the same
// line boxes the wrap path uses. Z-order: selection fills behind the glyph runs, caret fills after
// them. Selection is a translucent fill derived from the text color; the caret is the text color at
// full alpha, 1 px at device scale 1 and 2 px at scale >= 2. A blink phase below 0.5 hides only the
// caret; paint has no timers.
void paint_text_decoration(const PaintData& data, const rhi::Rect& content,
                           const text::TextStyle& text_style, const rhi::Color& color,
                           float opacity, bool wrap, PaintSink& sink, PaintStats& stats,
                           const PaintContext& context) {
    const std::string& text = data.text;
    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;

    text::TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = content.width;
    text::TextLayout local_layout = metrics_source->layout(text, text_style, constraints);
    if (data.max_lines > 0 && local_layout.lines.size() > data.max_lines) {
        local_layout.lines.resize(data.max_lines);
    }
    if (local_layout.lines.empty()) {
        return;
    }

    struct LineGeometry {
        text::TextLine line;
        float x = 0.0f;
        float top = 0.0f;
    };
    std::vector<LineGeometry> lines;
    lines.reserve(local_layout.lines.size());
    float line_top = content.y;
    for (const text::TextLine& line : local_layout.lines) {
        float line_x = content.x;
        if (data.text_align.value_or(PaintTextAlign::Start) == PaintTextAlign::Center) {
            line_x += (content.width - line.width) * 0.5f;
        } else if (data.text_align.value_or(PaintTextAlign::Start) == PaintTextAlign::End) {
            line_x += content.width - line.width;
        }
        lines.push_back(LineGeometry{line, std::max(content.x, line_x), line_top});
        line_top += line.height;
    }

    const TextDecoration& decoration = *data.decoration;
    std::size_t selection_begin = std::min(decoration.selection_begin, text.size());
    std::size_t selection_end = std::min(decoration.selection_end, text.size());
    if (selection_begin > selection_end) {
        std::swap(selection_begin, selection_end);
    }
    rhi::Color selection_color = color;
    selection_color.a = 0.35f * opacity; // fixed documented alpha; text color supplies RGB
    if (selection_end > selection_begin && selection_color.a > 0.0f) {
        for (const LineGeometry& geometry : lines) {
            const text::TextLine& line = geometry.line;
            const std::size_t start = std::max(selection_begin, line.begin);
            const std::size_t end = std::min(selection_end, line.end);
            if (end <= start) {
                continue;
            }
            const bool whole_line = selection_begin <= line.begin && selection_end >= line.end;
            const float start_width = metrics_source->advance(
                std::string_view(text).substr(line.begin, start - line.begin), text_style);
            float end_width =
                whole_line
                    ? line.width
                    : metrics_source->advance(
                          std::string_view(text).substr(line.begin, end - line.begin), text_style);
            end_width = std::max(end_width, start_width);
            sink.fill_rect({std::round(geometry.x + start_width), std::round(geometry.top),
                            std::round(end_width - start_width), line.height},
                           selection_color);
        }
    }

    // Glyph runs: selection above, text now (per line so the geometry matches the decoration).
    for (const LineGeometry& geometry : lines) {
        const text::TextLine& line = geometry.line;
        if (line.end > line.begin) {
            TextDraw run;
            run.rect = {std::round(geometry.x), std::round(geometry.top), line.width, line.height};
            run.text = std::string_view(text).substr(line.begin, line.end - line.begin);
            run.font_size = text_style.font_size;
            run.font_weight = text_style.font_weight;
            run.color = color;
            run.baseline = std::round(geometry.top + line.baseline);
            sink.draw_text(run);
            ++stats.texts;
        }
    }

    const bool caret_visible = decoration.caret_visible && decoration.blink_phase >= 0.5f;
    if (caret_visible) {
        const std::size_t caret = std::min(decoration.caret_byte_offset, text.size());
        const LineGeometry* caret_line = &lines.back();
        for (const LineGeometry& geometry : lines) {
            if (caret >= geometry.line.begin && caret <= geometry.line.end) {
                caret_line = &geometry;
                break;
            }
        }
        const std::size_t clamped = std::min(caret, caret_line->line.end);
        const float advance = metrics_source->advance(
            std::string_view(text).substr(caret_line->line.begin, clamped - caret_line->line.begin),
            text_style);
        const float caret_width = context.device_scale >= 2.0f ? 2.0f : 1.0f;
        sink.fill_rect({std::round(caret_line->x + advance), std::round(caret_line->top),
                        std::max(caret_width, 1.0f), caret_line->line.height},
                       color);
    }
}

void paint_text_lines(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                      PaintSink& sink, PaintStats& stats, const PaintContext& context) {
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
    if (data->decoration) {
        paint_text_decoration(*data, {content_x, content_y, content_width, content_height},
                              text_style, color, opacity, wrap, sink, stats, context);
        return;
    }
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

    text::TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = content_width;

    // The metrics source decides how wrap widths are measured: the deterministic stub keeps the
    // existing goldens byte-identical, a real-face source uses the same advances the glyph
    // renderer draws. Its id is part of the cache key, so the two can never mix.
    StubPaintTextMetrics stub_source;
    const PaintTextMetricsSource* metrics_source =
        context.text_metrics_source != nullptr ? context.text_metrics_source : &stub_source;

    // Caller-owned cache: a hit returns stable line boxes without recomputing or allocating; a
    // miss computes exactly what the uncached path would and stores the (already max_lines
    // truncated) result. Null cache keeps the uncached behavior.
    text::TextLayout local_layout;
    const text::TextLayout* layout = nullptr;
    if (context.text_layout_cache != nullptr && context.text_layout_cache->enabled()) {
        layout = &context.text_layout_cache->get(data->text, text_style, constraints,
                                                 *metrics_source, data->max_lines);
    } else {
        local_layout = metrics_source->layout(data->text, text_style, constraints);
        if (data->max_lines > 0 && local_layout.lines.size() > data->max_lines) {
            local_layout.lines.resize(data->max_lines);
        }
        layout = &local_layout;
    }

    const PaintTextAlign align = data->text_align.value_or(computed_text_align(style));
    float line_top = content_y;
    for (const text::TextLine& line : layout->lines) {
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

// A scroll container clips its children to its scrollport. When the scene owner set
// `scrollport_insets` (typically to the painted border widths), the clip uses them so clamping,
// painting, and hit testing agree; otherwise it falls back to the style-derived padding box, which
// keeps the default output byte-identical for borderless and bordered containers.
std::optional<rhi::Rect> scrollport_clip(const Node& node, const style::ComputedStyle& style,
                                         const rhi::Rect& rect) {
    if (!node.has_scrollport_insets()) {
        return detail::padding_box(style, rect);
    }
    const float width = rect.width - node.scrollport_left() - node.scrollport_right();
    const float height = rect.height - node.scrollport_top() - node.scrollport_bottom();
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return rhi::Rect{rect.x + node.scrollport_left(), rect.y + node.scrollport_top(), width,
                     height};
}

void paint_node(const Node& node, StyleProvider& styles, PaintSink& sink, PaintStats& stats,
                const PaintContext& context) {
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
    // world_transform(), and the clip keeps them inside the scrollport - the node's
    // `scrollport_insets` when set, otherwise the style padding box. The same box drives clamping
    // and hit testing (see scroll-and-gestures-v1.md 2.1).
    const bool scroll_clips = node.scrollable_x() || node.scrollable_y();
    const bool clip_children =
        (detail::clips_children(style) || scroll_clips) && rect.width > 0.0f && rect.height > 0.0f;
    bool pushed_clip = false;
    if (clip_children) {
        const std::optional<rhi::Rect> clip_rect =
            scroll_clips ? scrollport_clip(node, style, rect) : detail::padding_box(style, rect);
        if (clip_rect) {
            sink.push_clip(detail::corner_clip(style, *clip_rect));
            ++stats.clips;
            pushed_clip = true;
        }
    }
    // Text and vector runs are content: paint them after the element's own background/borders and
    // inside its own overflow/scroll clip, before the children (paint order). PaintKind selects one
    // active payload; the other is ignored.
    paint_text_lines(node, style, rect, sink, stats, context);
    detail::paint_vector_content(node, style, rect, sink, stats);
    for (const auto& child : node.children()) {
        paint_node(*child, styles, sink, stats, context);
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

PaintStats paint_scene(const Node& root, StyleProvider& styles, PaintSink& sink,
                       const PaintContext& context) {
    PaintStats stats;
    paint_node(root, styles, sink, stats, context);
    return stats;
}

} // namespace tessera::ui::paint
