// SPDX-License-Identifier: MIT

// The `background-image` paint path: a list of gradient layers (issue #614), each layer either a
// linear or a radial gradient (issue #613). The layer list is walked bottom to top, which is the
// CSS paint order, and each layer is resolved into the sink's own draw type.

#include "paint_gradient.hpp"
#include "paint_properties.hpp"
#include "tessera/ui/paint/background_layer_cache.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

namespace tessera::ui::paint::detail {
namespace {

using style::GradientStopValue;
using style::GradientValue;

// The resolved gradient in the paint pass's own terms: the stops are already clamped, sorted and
// opacity-folded, and the geometry is either an angle (linear) or a centre plus two radii (radial).
struct ResolvedGradient {
    GradientValue::Kind kind = GradientValue::Kind::Linear;
    std::array<LinearGradientStop, style::kMaxRepeatingStops> stops{};
    std::size_t stop_count = 0;
    float angle_degrees = 180.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float radius_x = 0.0f;
    float radius_y = 0.0f;
    // Conic only: the sweep's start angle, measured from 12 o'clock clockwise.
    float from_angle_degrees = 0.0f;
};

// Copies the authored stops into the resolved list, folding node opacity into each alpha. A
// repeating gradient's ramp is expanded first, so the paint path below never sees a second form.
// `count` is capped at the authored maximum: a payload set in code can carry a larger count than
// the value grammar allows.
void copy_stops(const GradientStopValue* source, std::size_t count, bool repeating, float opacity,
                ResolvedGradient& out) {
    std::array<GradientStopValue, style::kMaxRepeatingStops> expanded{};
    count = std::min(count, style::kMaxRepeatingStops);
    std::copy(source, source + count, expanded.begin());
    if (repeating) {
        // The expansion is in place: it rewrites the ramp so it already spans the whole gradient
        // line or radius, which is why the paint path below never sees a second form.
        count = style::expand_repeating_stops(expanded.data(), count);
    }
    out.stop_count = std::min(count, style::kMaxRepeatingStops);
    for (std::size_t index = 0; index < out.stop_count; ++index) {
        out.stops[index].offset = std::clamp(expanded[index].offset, 0.0f, 1.0f);
        const style::ColorRgba& color = expanded[index].color;
        out.stops[index].color = rhi::Color{color.r, color.g, color.b, color.a * opacity};
    }
    std::stable_sort(out.stops.begin(), out.stops.begin() + out.stop_count,
                     [](const LinearGradientStop& lhs, const LinearGradientStop& rhs) {
                         return lhs.offset < rhs.offset;
                     });
}

// One parsed layer value in the paint pass's terms. `rect` resolves the radial geometry, because a
// size keyword and a percentage position are relative to the box being painted.
ResolvedGradient resolve_value(const GradientValue& value, const rhi::Rect& rect, float opacity) {
    ResolvedGradient resolved;
    resolved.kind = value.kind;
    if (value.kind == GradientValue::Kind::Linear) {
        resolved.angle_degrees = value.linear.angle_degrees;
        copy_stops(value.linear.stops.data(), value.linear.stop_count, value.repeating, opacity,
                   resolved);
        return resolved;
    }
    if (value.kind == GradientValue::Kind::Conic) {
        const style::ConicGradientValue& conic = value.conic;
        resolved.center_x = rect.x + conic.position.x * rect.width;
        resolved.center_y = rect.y + conic.position.y * rect.height;
        resolved.from_angle_degrees = conic.from_angle_degrees;
        copy_stops(conic.stops.data(), conic.stop_count, value.repeating, opacity, resolved);
        return resolved;
    }
    const style::RadialGradientValue& radial = value.radial;
    resolved.center_x = rect.x + radial.position.x * rect.width;
    resolved.center_y = rect.y + radial.position.y * rect.height;
    // A size keyword resolves against the box: `farthest-corner` (the CSS default) reaches the
    // farthest corner from the centre, `closest-side` the nearest edge, and the explicit form is
    // already in px.
    const float dx = std::max(resolved.center_x - rect.x, rect.x + rect.width - resolved.center_x);
    const float dy = std::max(resolved.center_y - rect.y, rect.y + rect.height - resolved.center_y);
    const float near_x =
        std::min(resolved.center_x - rect.x, rect.x + rect.width - resolved.center_x);
    const float near_y =
        std::min(resolved.center_y - rect.y, rect.y + rect.height - resolved.center_y);
    switch (radial.size) {
    case style::RadialSize::Explicit:
        resolved.radius_x = radial.radius_x;
        resolved.radius_y = radial.radius_y;
        break;
    case style::RadialSize::ClosestSide:
        resolved.radius_x = near_x;
        resolved.radius_y = near_y;
        break;
    case style::RadialSize::FarthestSide:
        resolved.radius_x = dx;
        resolved.radius_y = dy;
        break;
    case style::RadialSize::ClosestCorner:
        resolved.radius_x = std::sqrt(near_x * near_x + near_y * near_y);
        resolved.radius_y = resolved.radius_x;
        break;
    case style::RadialSize::FarthestCorner:
    default:
        resolved.radius_x = std::sqrt(dx * dx + dy * dy);
        resolved.radius_y = resolved.radius_x;
        break;
    }
    if (radial.shape == style::RadialShape::Circle) {
        // A circle uses one radius; the larger axis keeps the whole ramp inside the box.
        resolved.radius_x = std::max(resolved.radius_x, resolved.radius_y);
        resolved.radius_y = resolved.radius_x;
    }
    copy_stops(radial.stops.data(), radial.stop_count, value.repeating, opacity, resolved);
    return resolved;
}

// A payload gradient (set in code) wins; otherwise the stylesheet's `background-image` feeds the
// list. Reading it from the resolved style keeps themes, `var()` and state rules working without
// the scene holding a second copy. Empty when there is nothing to paint.
style::BackgroundLayerList resolve_layers(const Node& node, const style::ComputedStyle& style,
                                          const rhi::Rect& rect,
                                          BackgroundLayerCache* const cache) {
    style::BackgroundLayerList list;
    const auto& data = node.paint_data();
    if (data && data->gradient) {
        // A payload may carry more stops than the value grammar allows (a caller can set any
        // count); the layer list is fixed-size, so clamp here rather than write past its array.
        const std::size_t stops = std::min(data->gradient->stop_count, style::kMaxGradientStops);
        style::BackgroundLayer& layer = list.layers[0];
        const bool conic = data->gradient->shape == GradientShape::Conic;
        layer.kind = conic                         ? style::BackgroundLayerKind::ConicGradient
                     : data->gradient->is_radial() ? style::BackgroundLayerKind::RadialGradient
                                                   : style::BackgroundLayerKind::LinearGradient;
        layer.gradient.kind = conic                         ? GradientValue::Kind::Conic
                              : data->gradient->is_radial() ? GradientValue::Kind::Radial
                                                            : GradientValue::Kind::Linear;
        layer.gradient.repeating = data->gradient->repeating;
        // The three shapes share one union slot (issue #731), so each branch constructs the member
        // it writes: a union member's default member initializers do not run when it is not the one
        // the constructor activated.
        if (conic) {
            style::ConicGradientValue value{};
            value.from_angle_degrees = data->gradient->from_angle_degrees;
            value.position.x = data->gradient->radial.center_x;
            value.position.y = data->gradient->radial.center_y;
            value.stop_count = stops;
            for (std::size_t index = 0; index < stops; ++index) {
                const LinearGradientStop& stop = data->gradient->stops[index];
                value.stops[index].offset = stop.offset;
                value.stops[index].color =
                    style::ColorRgba{stop.color.r, stop.color.g, stop.color.b, stop.color.a};
            }
            layer.gradient.conic = value;
        } else if (data->gradient->is_radial()) {
            style::RadialGradientValue radial{};
            radial.size = style::RadialSize::Explicit;
            radial.shape = style::RadialShape::Ellipse;
            // The payload's geometry is a fraction of the box, so it is resolved here; the
            // stylesheet's explicit radii are already px and are used as they are.
            radial.radius_x = data->gradient->radial.radius_x * rect.width;
            radial.radius_y = data->gradient->radial.radius_y * rect.height;
            radial.position.x = data->gradient->radial.center_x;
            radial.position.y = data->gradient->radial.center_y;
            radial.stop_count = stops;
            for (std::size_t index = 0; index < stops; ++index) {
                const LinearGradientStop& stop = data->gradient->stops[index];
                radial.stops[index].offset = stop.offset;
                radial.stops[index].color =
                    style::ColorRgba{stop.color.r, stop.color.g, stop.color.b, stop.color.a};
            }
            layer.gradient.radial = radial;
        } else {
            style::LinearGradientValue linear{};
            linear.angle_degrees = data->gradient->angle_degrees;
            linear.stop_count = stops;
            for (std::size_t index = 0; index < stops; ++index) {
                const LinearGradientStop& stop = data->gradient->stops[index];
                linear.stops[index].offset = stop.offset;
                linear.stops[index].color =
                    style::ColorRgba{stop.color.r, stop.color.g, stop.color.b, stop.color.a};
            }
            layer.gradient.linear = linear;
        }
        list.count = 1;
        return list;
    }
    const std::string* image = style.find("background-image");
    if (image == nullptr) {
        return list;
    }
    // One parse per *style*, not per painted node per frame (issue #733 item 2). A null cache is
    // the uncached path and parses every call, byte-identical to before the cache existed.
    if (cache != nullptr) {
        return cache->get(style, *image).parsed.value_or(list);
    }
    const std::optional<style::BackgroundLayerList> parsed = style::parse_background_layers(*image);
    return parsed.value_or(list);
}

// Walks a resolved layer list bottom to top and emits each gradient. Shared by the
// `background-image` paint and the `mask-image` paint (issue #617): the same grammar and the same
// draws, only the property read and the destination (the node's background, or a layer's mask
// target) differ.
void emit_layers(const style::BackgroundLayerList& list, const rhi::Rect& rect,
                 const RoundedClip& clip, float opacity, PaintSink& sink, PaintStats& stats) {
    // CSS paints the LAST layer first and the FIRST layer on top (Backgrounds 3 section 2.1), so
    // the list is walked backwards. Each layer is a full-box fill, so a later (lower) layer is only
    // visible where the layers above it are translucent.
    for (std::size_t index = list.count; index-- > 0;) {
        const style::BackgroundLayer& layer = list.layers[index];
        if (layer.kind == style::BackgroundLayerKind::None) {
            continue; // a transparent layer paints nothing
        }
        const ResolvedGradient gradient = resolve_value(layer.gradient, rect, opacity);
        if (gradient.stop_count == 0) {
            continue;
        }
        if (gradient.kind == GradientValue::Kind::Linear) {
            GradientDraw draw;
            draw.rect = rect;
            draw.clip = clip;
            draw.angle_degrees = gradient.angle_degrees;
            for (std::size_t stop = 0; stop < gradient.stop_count; ++stop) {
                draw.stops[stop] = gradient.stops[stop];
            }
            draw.stop_count = gradient.stop_count;
            sink.fill_linear_gradient(draw);
            ++stats.gradients;
            continue;
        }
        if (gradient.kind == GradientValue::Kind::Conic) {
            ConicGradientDraw draw;
            draw.rect = rect;
            draw.clip = clip;
            draw.center_x = gradient.center_x;
            draw.center_y = gradient.center_y;
            draw.from_angle_degrees = gradient.from_angle_degrees;
            for (std::size_t stop = 0; stop < gradient.stop_count; ++stop) {
                draw.stops[stop] = gradient.stops[stop];
            }
            draw.stop_count = gradient.stop_count;
            sink.fill_conic_gradient(draw);
            ++stats.gradients;
            continue;
        }
        RadialGradientDraw draw;
        draw.rect = rect;
        draw.clip = clip;
        draw.center_x = gradient.center_x;
        draw.center_y = gradient.center_y;
        draw.radius_x = gradient.radius_x;
        draw.radius_y = gradient.radius_y;
        for (std::size_t stop = 0; stop < gradient.stop_count; ++stop) {
            draw.stops[stop] = gradient.stops[stop];
        }
        draw.stop_count = gradient.stop_count;
        sink.fill_radial_gradient(draw);
        ++stats.gradients;
    }
}

} // namespace

void paint_gradient(const Node& node, const style::ComputedStyle& style, const rhi::Rect& rect,
                    float opacity, PaintSink& sink, PaintStats& stats,
                    BackgroundLayerCache* const cache) {
    if (opacity <= 0.0f || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    const style::BackgroundLayerList list = resolve_layers(node, style, rect, cache);
    if (list.empty()) {
        return;
    }
    emit_layers(list, rect, corner_clip(style, rect), opacity, sink, stats);
}

bool has_mask_image(const style::ComputedStyle& style) {
    const std::string* image = style.find("mask-image");
    if (image == nullptr) {
        return false;
    }
    const std::optional<style::BackgroundLayerList> parsed = style::parse_background_layers(*image);
    return parsed.has_value() && !parsed->empty() &&
           std::any_of(parsed->layers.begin(), parsed->layers.begin() + parsed->count,
                       [](const style::BackgroundLayer& layer) {
                           return layer.kind != style::BackgroundLayerKind::None;
                       });
}

void paint_mask_image(const style::ComputedStyle& style, const rhi::Rect& rect, PaintSink& sink,
                      PaintStats& stats) {
    const std::string* image = style.find("mask-image");
    if (image == nullptr || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }
    const std::optional<style::BackgroundLayerList> parsed = style::parse_background_layers(*image);
    if (!parsed.has_value() || parsed->empty()) {
        return;
    }
    // mask-clip / mask-origin default to border-box, which is not rounded by border-radius, and the
    // mask's own alpha is the whole value, so there is no node opacity to fold in.
    emit_layers(*parsed, rect, RoundedClip{rect, 0.0f, 0.0f, 0.0f, 0.0f}, 1.0f, sink, stats);
}

} // namespace tessera::ui::paint::detail
