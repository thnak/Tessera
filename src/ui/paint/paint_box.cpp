// SPDX-License-Identifier: MIT

#include "paint_box.hpp"
#include "paint_frame.hpp"
#include "paint_properties.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace tessera::ui::paint::detail {

static void emit_rect(PaintSink& sink, const rhi::Rect& rect,
                      const std::optional<rhi::Color>& color, float opacity, std::size_t& counter) {
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
                   const rhi::Color& current, float device_scale, PaintSink& sink,
                   PaintStats& stats) {
    const detail::BorderWidths widths =
        detail::snap_border_widths(detail::border_widths(style, rect), device_scale);
    const float top = widths.top;
    const float right = widths.right;
    const float bottom = widths.bottom;
    const float left = widths.left;

    const float middle_height = rect.height - top - bottom;
    const std::array<std::optional<rhi::Color>, 4> colors = detail::border_colors(style, current);

    emit_rect(sink, rhi::Rect{rect.x, rect.y, rect.width, top}, colors[0], opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x + rect.width - right, rect.y + top, right, middle_height},
              colors[1], opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + rect.height - bottom, rect.width, bottom}, colors[2],
              opacity, stats.borders);
    emit_rect(sink, rhi::Rect{rect.x, rect.y + top, left, middle_height}, colors[3], opacity,
              stats.borders);
}

// A border on a rounded box. The edge strips are rectangles and cannot follow a corner, so a
// uniform border (one width, one colour, one radius) is drawn as a ring: the rounded box minus the
// same box inset by the width, anti-aliased on both edges by the sink. A border with different
// widths or colours per side keeps the edge strips.
static bool same_rgba(const rhi::Color& lhs, const rhi::Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

std::optional<RingBorder> rounded_ring(const style::ComputedStyle& style, const rhi::Rect& rect,
                                       float opacity, const rhi::Color& current,
                                       float device_scale) {
    const std::optional<float> radius = detail::uniform_radius(style);
    if (!radius || *radius <= 0.0f) {
        return std::nullopt;
    }
    const detail::BorderWidths widths =
        detail::snap_border_widths(detail::border_widths(style, rect), device_scale);
    if (widths.top <= 0.0f || widths.top != widths.right || widths.top != widths.bottom ||
        widths.top != widths.left) {
        return std::nullopt;
    }
    const std::array<std::optional<rhi::Color>, 4> colors = detail::border_colors(style, current);
    if (!colors[0] || !colors[1] || !colors[2] || !colors[3] ||
        !same_rgba(*colors[0], *colors[1]) || !same_rgba(*colors[0], *colors[2]) ||
        !same_rgba(*colors[0], *colors[3])) {
        return std::nullopt;
    }
    rhi::Color color = *colors[0];
    color.a *= opacity;
    if (color.a <= 0.0f) {
        return std::nullopt;
    }
    return RingBorder{color, widths.top, clamp_radius(*radius, rect)};
}

} // namespace tessera::ui::paint::detail
