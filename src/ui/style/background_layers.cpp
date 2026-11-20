// SPDX-License-Identifier: MIT

// The `background-image` layer list (issue #614) with both gradient shapes (issue #613).
//
// A layer is `none` or a gradient. The gradient is the whole `GradientValue`, so the shape tag and
// the repeating flag inside it are authoritative and this module never has to know which shape a
// layer carries beyond "is it a gradient at all".

#include "tessera/ui/style/background_layers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Splits at commas outside parentheses, so a gradient's own commas stay inside its layer.
std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        const bool at_comma = i < text.size() && text[i] == ',' && depth == 0;
        if (i == text.size() || at_comma) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

std::string format_number(float value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return std::string(buffer);
}

// The signed difference between two angles in (-180, 180], so an angle interpolates along the
// shorter arc: 350deg to 10deg moves 20 degrees forward, not 340 back.
float wrap_degrees(float degrees) {
    float value = std::fmod(degrees, 360.0f);
    if (value > 180.0f) {
        value -= 360.0f;
    } else if (value <= -180.0f) {
        value += 360.0f;
    }
    return value;
}

// A `none` layer is the transparent image: the same gradient with every stop's alpha at 0. Pairing
// a gradient with it is what makes a layer fade in or out, and it keeps the stop count and the
// geometry of the layer it pairs with so the ramp does not slide while it fades.
BackgroundLayer transparent_like(const BackgroundLayer& layer) {
    BackgroundLayer result = layer;
    if (result.kind == BackgroundLayerKind::None) {
        return result;
    }
    GradientValue& gradient = result.gradient;
    const std::size_t count = gradient.kind == GradientValue::Kind::Linear
                                  ? gradient.linear.stop_count
                                  : gradient.radial.stop_count;
    for (std::size_t index = 0; index < count; ++index) {
        GradientStopValue& stop = gradient.kind == GradientValue::Kind::Linear
                                      ? gradient.linear.stops[index]
                                      : gradient.radial.stops[index];
        stop.color.a = 0.0f;
    }
    return result;
}

// One gradient layer at progress `t`. Stops pair up positionally; the shorter list repeats its last
// stop, which is the CSS rule for a stop-count mismatch (CSS Images 4 section 3.1). A shape change
// between the two sides is discrete: the ramp switches at the halfway point rather than pretending
// a linear angle is a radial centre.
GradientValue blend_gradients(const GradientValue& from, const GradientValue& to, float t) {
    if (from.kind != to.kind) {
        return t < 0.5f ? from : to;
    }
    GradientValue result = from;
    if (from.kind == GradientValue::Kind::Linear) {
        const LinearGradientValue& a = from.linear;
        const LinearGradientValue& b = to.linear;
        if (a.stop_count == 0 || b.stop_count == 0) {
            return a.stop_count == 0 ? to : from;
        }
        result.linear.angle_degrees = std::fmod(
            std::fmod(a.angle_degrees + wrap_degrees(b.angle_degrees - a.angle_degrees) * t,
                      360.0f) +
                360.0f,
            360.0f);
        result.linear.stop_count = std::max(a.stop_count, b.stop_count);
        for (std::size_t index = 0; index < result.linear.stop_count; ++index) {
            const GradientStopValue& lhs = a.stops[std::min(index, a.stop_count - 1)];
            const GradientStopValue& rhs = b.stops[std::min(index, b.stop_count - 1)];
            result.linear.stops[index].offset = lhs.offset + (rhs.offset - lhs.offset) * t;
            result.linear.stops[index].color = interpolate_color(lhs.color, rhs.color, t);
        }
        return result;
    }
    const RadialGradientValue& a = from.radial;
    const RadialGradientValue& b = to.radial;
    if (a.stop_count == 0 || b.stop_count == 0) {
        return a.stop_count == 0 ? to : from;
    }
    // The geometry interpolates only when both sides name it the same way; a size keyword change is
    // discrete for the same reason a shape change is.
    if (a.size == b.size && a.shape == b.shape) {
        result.radial.radius_x = a.radius_x + (b.radius_x - a.radius_x) * t;
        result.radial.radius_y = a.radius_y + (b.radius_y - a.radius_y) * t;
        result.radial.position.x = a.position.x + (b.position.x - a.position.x) * t;
        result.radial.position.y = a.position.y + (b.position.y - a.position.y) * t;
    } else if (t >= 0.5f) {
        result.radial = b;
    }
    result.radial.stop_count = std::max(a.stop_count, b.stop_count);
    for (std::size_t index = 0; index < result.radial.stop_count; ++index) {
        const GradientStopValue& lhs = a.stops[std::min(index, a.stop_count - 1)];
        const GradientStopValue& rhs = b.stops[std::min(index, b.stop_count - 1)];
        result.radial.stops[index].offset = lhs.offset + (rhs.offset - lhs.offset) * t;
        result.radial.stops[index].color = interpolate_color(lhs.color, rhs.color, t);
    }
    return result;
}

BackgroundLayer blend_layers(const BackgroundLayer& from, const BackgroundLayer& to, float t) {
    if (from.kind == BackgroundLayerKind::None && to.kind == BackgroundLayerKind::None) {
        return BackgroundLayer{};
    }
    // A `none` on either side pairs as the transparent form of the other layer, so the pair is
    // always two gradients and the fade is a ramp of alpha rather than a discrete switch.
    const BackgroundLayer& lhs =
        from.kind == BackgroundLayerKind::None ? transparent_like(to) : from;
    const BackgroundLayer& rhs = to.kind == BackgroundLayerKind::None ? transparent_like(from) : to;
    BackgroundLayer result;
    result.kind = lhs.kind;
    result.gradient = blend_gradients(lhs.gradient, rhs.gradient, t);
    return result;
}

} // namespace

std::optional<BackgroundLayerList> parse_background_layers(std::string_view text) {
    const std::string_view body = trim(text);
    if (body.empty()) {
        return std::nullopt;
    }
    const std::vector<std::string_view> parts = split_commas(body);
    if (parts.empty() || parts.size() > kMaxBackgroundLayers) {
        return std::nullopt;
    }

    BackgroundLayerList list;
    for (const std::string_view part : parts) {
        if (part.empty()) {
            return std::nullopt;
        }
        BackgroundLayer layer;
        if (lower(part) == "none") {
            layer.kind = BackgroundLayerKind::None;
        } else if (const std::optional<GradientValue> gradient = parse_gradient(part)) {
            layer.kind = gradient->kind == GradientValue::Kind::Linear
                             ? BackgroundLayerKind::LinearGradient
                             : BackgroundLayerKind::RadialGradient;
            layer.gradient = *gradient;
        } else {
            // `url()`, a conic gradient or a malformed stop list: outside this grammar, so the whole
            // value is rejected rather than painted as a guess.
            return std::nullopt;
        }
        list.layers[list.count++] = layer;
    }
    return list;
}

std::string format_background_layers(const BackgroundLayerList& list) {
    std::string out;
    for (std::size_t index = 0; index < list.count; ++index) {
        if (index != 0) {
            out += ", ";
        }
        const BackgroundLayer& layer = list.layers[index];
        if (layer.kind == BackgroundLayerKind::None) {
            out += "none";
            continue;
        }
        const GradientValue& gradient = layer.gradient;
        const bool linear = gradient.kind == GradientValue::Kind::Linear;
        out += gradient.repeating ? "repeating-" : "";
        out += linear ? "linear-gradient(" : "radial-gradient(";
        if (linear) {
            out += format_number(gradient.linear.angle_degrees);
            out += "deg";
        } else {
            // The geometry is printed in the explicit form so a round trip is exact; a keyword form
            // would have to be re-resolved against a box the formatter does not have.
            out += format_number(gradient.radial.radius_x);
            out += "px ";
            out += format_number(gradient.radial.radius_y);
            out += "px at ";
            out += format_number(gradient.radial.position.x * 100.0f);
            out += "% ";
            out += format_number(gradient.radial.position.y * 100.0f);
            out += '%';
        }
        const std::size_t count =
            linear ? gradient.linear.stop_count : gradient.radial.stop_count;
        for (std::size_t stop = 0; stop < count; ++stop) {
            const GradientStopValue& value =
                linear ? gradient.linear.stops[stop] : gradient.radial.stops[stop];
            out += ", ";
            out += format_color(value.color);
            out.push_back(' ');
            out += format_number(value.offset * 100.0f);
            out.push_back('%');
        }
        out.push_back(')');
    }
    return out;
}

std::optional<std::string> interpolate_background_layers(std::string_view from, std::string_view to,
                                                         double t) {
    const std::optional<BackgroundLayerList> lhs = parse_background_layers(from);
    const std::optional<BackgroundLayerList> rhs = parse_background_layers(to);
    if (!lhs || !rhs) {
        return std::nullopt;
    }
    const float fraction =
        static_cast<float>(std::isfinite(t) ? t : 0.0); // extrapolates, like every other property

    // CSS pads the shorter list at the bottom (the end of the list), so a layer added on top of a
    // shorter list fades in over the layers that already exist instead of shifting them.
    const std::size_t count = std::max(lhs->count, rhs->count);
    BackgroundLayerList result;
    result.count = count;
    for (std::size_t index = 0; index < count; ++index) {
        const BackgroundLayer missing;
        const BackgroundLayer& a = index < lhs->count ? lhs->layers[index] : missing;
        const BackgroundLayer& b = index < rhs->count ? rhs->layers[index] : missing;
        result.layers[index] = blend_layers(a, b, fraction);
    }
    return format_background_layers(result);
}

} // namespace tessera::ui::style
