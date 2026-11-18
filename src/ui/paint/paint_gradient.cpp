// SPDX-License-Identifier: MIT

#include "paint_gradient.hpp"
#include "paint_properties.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace tessera::ui::paint::detail {
namespace {

// A payload gradient (set in code) wins; otherwise the stylesheet's `background-image` feeds one.
// Reading it from the resolved style keeps themes, `var()` and state rules working without the
// scene holding a second copy. Empty (zero stops) when there is nothing to paint.
LinearGradient resolve_gradient(const Node& node, const style::ComputedStyle& style) {
    const auto& data = node.paint_data();
    if (data && data->gradient) {
        return *data->gradient;
    }
    LinearGradient gradient;
    const std::string* image = style.find("background-image");
    if (image == nullptr) {
        return gradient;
    }
    const std::optional<style::LinearGradientValue> parsed = style::parse_linear_gradient(*image);
    if (!parsed) {
        return gradient;
    }
    gradient.angle_degrees = parsed->angle_degrees;
    gradient.stop_count = parsed->stop_count;
    for (std::size_t index = 0; index < parsed->stop_count; ++index) {
        const style::ColorRgba& color = parsed->stops[index].color;
        gradient.stops[index].offset = parsed->stops[index].offset;
        gradient.stops[index].color = rhi::Color{color.r, color.g, color.b, color.a};
    }
    return gradient;
}

} // namespace

void paint_linear_gradient(const Node& node, const style::ComputedStyle& style,
                           const rhi::Rect& rect, float opacity, PaintSink& sink,
                           PaintStats& stats) {
    if (opacity <= 0.0f) {
        return;
    }
    LinearGradient gradient = resolve_gradient(node, style);
    if (gradient.stop_count == 0) {
        return;
    }
    if (gradient.stop_count > LinearGradient::kMaxStops) {
        gradient.stop_count = LinearGradient::kMaxStops;
    }
    for (std::size_t index = 0; index < gradient.stop_count; ++index) {
        gradient.stops[index].offset = std::clamp(gradient.stops[index].offset, 0.0f, 1.0f);
    }
    std::stable_sort(gradient.stops.begin(), gradient.stops.begin() + gradient.stop_count,
                     [](const LinearGradientStop& lhs, const LinearGradientStop& rhs) {
                         return lhs.offset < rhs.offset;
                     });
    for (std::size_t index = 0; index < gradient.stop_count; ++index) {
        gradient.stops[index].color.a *= opacity;
    }
    GradientDraw draw;
    draw.rect = rect;
    draw.clip = corner_clip(style, rect);
    draw.angle_degrees = gradient.angle_degrees;
    draw.stops = gradient.stops;
    draw.stop_count = gradient.stop_count;
    sink.fill_linear_gradient(draw);
    ++stats.gradients;
}

} // namespace tessera::ui::paint::detail
