// SPDX-License-Identifier: MIT
//
// Caret box and point-to-caret hit for an editable text node (docs/design/canvas-text-input.md
// section 8). The geometry is `paint_text_lines`'s, resolved the same way: padding, the wrap
// decision, the single-line vertical centring and the per-line alignment. It lives here, not in
// `paint_text.cpp`, which is at its size budget; the shared pieces (`text_wraps`,
// `computed_text_align`, `caret_box`) are called, not copied.

#include "paint_properties.hpp"
#include "paint_text_flex.hpp"
#include "tessera/ui/paint/text_field_geometry.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <algorithm>
#include <vector>

namespace tessera::ui::paint {
namespace {

struct LineBox {
    text::TextLine line;
    float x = 0.0f;
    float top = 0.0f;
};

struct FieldGeometry {
    text::TextStyle style;
    rhi::Rect content;
    text::TextLayout layout;
    std::vector<LineBox> lines;
    const PaintData* data = nullptr;
};

// `source` is never null here: the callers substitute the stub.
std::optional<FieldGeometry> resolve(const Node& node, const style::ComputedStyle& style,
                                     const rhi::Rect& rect, const PaintTextMetricsSource& source) {
    const std::optional<PaintData>& data = node.paint_data();
    if (!data || data->kind != PaintKind::Text) {
        return std::nullopt;
    }
    FieldGeometry geometry;
    geometry.data = &*data;
    geometry.style = text::text_style_from(style);
    if (data->text_style.font_size > 0.0f) {
        geometry.style.font_size = data->text_style.font_size;
    }
    if (data->text_style.font_weight > 0) {
        geometry.style.font_weight = data->text_style.font_weight;
    }
    const float left = detail::length_value(style, "padding-left").value_or(0.0f);
    const float right = detail::length_value(style, "padding-right").value_or(0.0f);
    const float top = detail::length_value(style, "padding-top").value_or(0.0f);
    const float bottom = detail::length_value(style, "padding-bottom").value_or(0.0f);
    float width = std::max(0.0f, rect.width - left - right);
    float height = std::max(0.0f, rect.height - top - bottom);
    float y = rect.y + top;
    if (width <= 0.0f || height <= 0.0f || geometry.style.font_size <= 0.0f) {
        return std::nullopt;
    }
    const bool wrap = text_wraps(node.element_kind(), *data, geometry.style.white_space, source,
                                 geometry.style, height);
    const FlexTextCentering flex = flex_text_centering(node.element_kind(), style);
    PaintTextAlign align = data->text_align.value_or(computed_text_align(style));
    align = flex.horizontal ? PaintTextAlign::Center : align;
    if (((node.element_kind() == "TextInput" || node.element_kind() == "Button") && !wrap) ||
        flex.vertical) {
        text::TextConstraints one_line;
        one_line.wrap = wrap;
        one_line.max_width = width;
        const text::TextLayout probe = source.layout(
            data->text.empty() ? std::string("x") : data->text, geometry.style, one_line);
        float lines_height = 0.0f;
        for (const text::TextLine& line : probe.lines) {
            lines_height += line.height;
        }
        if (!probe.lines.empty()) {
            const float offset = std::max(0.0f, (height - lines_height) * 0.5f);
            y += offset;
            height = std::max(0.0f, height - offset);
        }
    }
    y -= geometry.style.baseline_shift;
    geometry.content = {rect.x + left, y, width, height};

    text::TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = width;
    geometry.layout = source.layout(data->text, geometry.style, constraints);
    if (data->max_lines > 0 && geometry.layout.lines.size() > data->max_lines) {
        geometry.layout.lines.resize(data->max_lines);
    }
    if (geometry.layout.lines.empty()) {
        // An empty field still has one line: the caret sits on it.
        const text::FontMetrics fallback = source.metrics(geometry.style);
        text::TextLine line;
        line.height = fallback.ascent + fallback.descent + fallback.line_gap;
        line.baseline = fallback.ascent;
        geometry.layout.lines.push_back(line);
    }
    float line_top = geometry.content.y;
    for (const text::TextLine& line : geometry.layout.lines) {
        float line_x = geometry.content.x;
        if (align == PaintTextAlign::Center) {
            line_x += (width - line.width) * 0.5f;
        } else if (align == PaintTextAlign::End) {
            line_x += width - line.width;
        }
        geometry.lines.push_back({line, std::max(geometry.content.x, line_x), line_top});
        line_top += line.height;
    }
    return geometry;
}

float advance_of(std::string_view run, const text::TextStyle& style, const void* user) {
    return static_cast<const PaintTextMetricsSource*>(user)->advance(run, style);
}

} // namespace

std::optional<CaretBox> text_field_caret_box(const Node& node, const style::ComputedStyle& style,
                                             const rhi::Rect& rect, std::size_t byte_offset,
                                             const PaintTextMetricsSource* source,
                                             float device_scale) {
    const StubPaintTextMetrics stub;
    const PaintTextMetricsSource& metrics = source != nullptr ? *source : stub;
    const std::optional<FieldGeometry> field = resolve(node, style, rect, metrics);
    if (!field) {
        return std::nullopt;
    }
    const std::string& text = field->data->text;
    const std::size_t caret = std::min(byte_offset, text.size());
    // The first line whose range holds the caret (a line break matches two: the first is where it
    // was typed), else the last, as the paint loop picks it.
    const LineBox* on = &field->lines.back();
    for (const LineBox& line : field->lines) {
        if (caret >= line.line.begin && caret <= line.line.end) {
            on = &line;
            break;
        }
    }
    return caret_box(text, field->layout, caret, field->style, metrics, device_scale, on->x,
                     on->top);
}

std::optional<text::TextHit> text_field_hit(const Node& node, const style::ComputedStyle& style,
                                            const rhi::Rect& rect, float x, float y,
                                            const PaintTextMetricsSource* source) {
    const StubPaintTextMetrics stub;
    const PaintTextMetricsSource& metrics = source != nullptr ? *source : stub;
    const std::optional<FieldGeometry> field = resolve(node, style, rect, metrics);
    if (!field) {
        return std::nullopt;
    }
    // The line by y (clamped to the first and last), then the point inside that line alone,
    // measured with the painted advances: the text hit test never lays the whole field out a second
    // way.
    const LineBox* on = &field->lines.back();
    for (const LineBox& line : field->lines) {
        if (y < line.top + line.line.height) {
            on = &line;
            break;
        }
    }
    const std::string& text = field->data->text;
    const std::size_t begin = std::min(on->line.begin, text.size());
    const std::size_t end = std::min(std::max(on->line.end, begin), text.size());
    text::TextConstraints one_line;
    one_line.wrap = false;
    const text::TextHit local = text::hit_test_point(
        std::string_view(text).substr(begin, end - begin), field->style, one_line, x - on->x, 0.0f,
        metrics.metrics(field->style), &advance_of, &metrics);
    return text::TextHit{begin + local.byte_offset, local.affinity};
}

} // namespace tessera::ui::paint
