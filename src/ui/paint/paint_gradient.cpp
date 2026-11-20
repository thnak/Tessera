// SPDX-License-Identifier: MIT

#include "paint_gradient.hpp"
#include "paint_properties.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace tessera::ui::paint::detail {
namespace {

// One gradient layer, clamped and sorted, with the node opacity folded into its stop alphas.
LinearGradient resolve_layer(const style::LinearGradientValue& value, float opacity) {
    LinearGradient gradient;
    gradient.angle_degrees = value.angle_degrees;
    gradient.stop_count = std::min(value.stop_count, LinearGradient::kMaxStops);
    for (std::size_t index = 0; index < gradient.stop_count; ++index) {
        const style::ColorRgba& color = value.stops[index].color;
        gradient.stops[index].offset = std::clamp(value.stops[index].offset, 0.0f, 1.0f);
        gradient.stops[index].color = rhi::Color{color.r, color.g, color.b, color.a * opacity};
    }
    std::stable_sort(gradient.stops.begin(), gradient.stops.begin() + gradient.stop_count,
                     [](const LinearGradientStop& lhs, const LinearGradientStop& rhs) {
                         return lhs.offset < rhs.offset;
                     });
    return gradient;
}

// A payload gradient (set in code) wins; otherwise the stylesheet's `background-image` feeds the
// list. Reading it from the resolved style keeps themes, `var()` and state rules working without
// the scene holding a second copy. Empty when there is nothing to paint.
style::BackgroundLayerList resolve_layers(const Node& node, const style::ComputedStyle& style) {
    style::BackgroundLayerList list;
    const auto& data = node.paint_data();
    if (data && data->gradient) {
        // A payload may carry more stops than the value grammar allows (a caller can set any
        // count); the layer list is fixed-size, so clamp here rather than write past its array.
        const std::size_t stops = std::min(data->gradient->stop_count, style::kMaxGradientStops);
        list.count = 1;
        list.layers[0].kind = style::BackgroundLayerKind::LinearGradient;
        list.layers[0].gradient.angle_degrees = data->gradient->angle_degrees;
        list.layers[0].gradient.stop_count = stops;
        for (std::size_t index = 0; index < stops; ++index) {
            const LinearGradientStop& stop = data->gradient->stops[index];
            list.layers[0].gradient.stops[index].offset = stop.offset;
            list.layers[0].gradient.stops[index].color =
                style::ColorRgba{stop.color.r, stop.color.g, stop.color.b, stop.color.a};
        }
        return list;
    }
    const std::string* image = style.find("background-image");
    if (image == nullptr) {
        return list;
    }
    const std::optional<style::BackgroundLayerList> parsed = style::parse_background_layers(*image);
    return parsed.value_or(list);
}

} // namespace

void paint_linear_gradient(const Node& node, const style::ComputedStyle& style,
                           const rhi::Rect& rect, float opacity, PaintSink& sink,
                           PaintStats& stats) {
    if (opacity <= 0.0f || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    const style::BackgroundLayerList list = resolve_layers(node, style);
    if (list.empty()) {
        return;
    }
    const RoundedClip clip = corner_clip(style, rect);
    // CSS paints the LAST layer first and the FIRST layer on top (Backgrounds 3 section 2.1), so
    // the list is walked backwards. Each layer is a full-box fill, so a later (lower) layer is only
    // visible where the layers above it are translucent.
    for (std::size_t index = list.count; index-- > 0;) {
        const style::BackgroundLayer& layer = list.layers[index];
        if (layer.kind != style::BackgroundLayerKind::LinearGradient) {
            continue; // `none`: a transparent layer paints nothing
        }
        const LinearGradient gradient = resolve_layer(layer.gradient, opacity);
        if (gradient.stop_count == 0) {
            continue;
        }
        GradientDraw draw;
        draw.rect = rect;
        draw.clip = clip;
        draw.angle_degrees = gradient.angle_degrees;
        draw.stops = gradient.stops;
        draw.stop_count = gradient.stop_count;
        sink.fill_linear_gradient(draw);
        ++stats.gradients;
    }
}

} // namespace tessera::ui::paint::detail
