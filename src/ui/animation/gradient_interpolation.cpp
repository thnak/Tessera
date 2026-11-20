// SPDX-License-Identifier: MIT

#include "gradient_interpolation.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tessera::ui::animation {
namespace {

using style::ColorRgba;
using style::GradientStopValue;
using style::GradientValue;

float lerp(float from, float to, double t) {
    return static_cast<float>(from + (to - from) * t);
}

// `%.4f` with trailing zeros trimmed, so an interpolated value is stable text (the same rule the
// box-shadow interpolator uses).
std::string format_number(float value) {
    if (std::abs(value) < 0.00005f) {
        value = 0.0f;
    }
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
    std::string result(buffer);
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result.empty() ? "0" : result;
}

std::string format_percentage(float fraction) {
    return format_number(fraction * 100.0f) + "%";
}

// One interpolated stop list. The shorter list is padded by repeating its last stop at the same
// offset, which is CSS's rule for a mismatched stop count: the extra stops of the longer list hold
// their colour instead of fading toward an invented one.
std::vector<GradientStopValue> blend_stops(const GradientStopValue* from, std::size_t from_count,
                                           const GradientStopValue* to, std::size_t to_count,
                                           double t) {
    const std::size_t count = std::max(from_count, to_count);
    std::vector<GradientStopValue> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const GradientStopValue& a = from[std::min(index, from_count - 1)];
        const GradientStopValue& b = to[std::min(index, to_count - 1)];
        GradientStopValue stop;
        stop.offset = lerp(a.offset, b.offset, t);
        stop.color = style::interpolate_color(a.color, b.color, static_cast<float>(t));
        result.push_back(stop);
    }
    return result;
}

std::string format_stops(const std::vector<GradientStopValue>& stops) {
    std::string text;
    for (const GradientStopValue& stop : stops) {
        if (!text.empty()) {
            text += ", ";
        }
        text += style::format_color(stop.color) + " " + format_percentage(stop.offset);
    }
    return text;
}

std::string format_linear(const GradientValue& gradient,
                          const std::vector<GradientStopValue>& stops, double t,
                          const GradientValue& other) {
    const float angle = lerp(gradient.linear.angle_degrees, other.linear.angle_degrees, t);
    return "linear-gradient(" + format_number(angle) + "deg, " + format_stops(stops) + ")";
}

std::string format_radial(const GradientValue& gradient,
                          const std::vector<GradientStopValue>& stops, double t,
                          const GradientValue& other) {
    const auto& a = gradient.radial;
    const auto& b = other.radial;
    // The shape is discrete: a circle and an ellipse have no in-between, so the pair is only
    // interpolable when the shapes match (checked by the caller). The size keyword is likewise
    // discrete; the resolved radii are what actually blend, so the output names explicit radii.
    const float radius_x = lerp(a.radius_x, b.radius_x, t);
    const float radius_y = lerp(a.radius_y, b.radius_y, t);
    const float center_x = lerp(a.position.x, b.position.x, t);
    const float center_y = lerp(a.position.y, b.position.y, t);
    std::string text = "radial-gradient(";
    text += a.shape == style::RadialShape::Circle ? "circle " : "ellipse ";
    text += format_number(radius_x) + "px " + format_number(radius_y) + "px at " +
            format_percentage(center_x) + " " + format_percentage(center_y) + ", ";
    text += format_stops(stops) + ")";
    return text;
}

} // namespace

std::optional<std::string> interpolate_gradient(std::string_view from, std::string_view to,
                                                double t) {
    if (!std::isfinite(t)) {
        return std::nullopt;
    }
    const std::optional<GradientValue> lhs = style::parse_gradient(from);
    const std::optional<GradientValue> rhs = style::parse_gradient(to);
    if (!lhs || !rhs) {
        return std::nullopt;
    }
    // A shape change or a repeating mismatch is discrete, not interpolable.
    if (lhs->kind != rhs->kind || lhs->repeating != rhs->repeating) {
        return std::nullopt;
    }
    if (lhs->kind == GradientValue::Kind::Radial && lhs->radial.shape != rhs->radial.shape) {
        return std::nullopt;
    }
    if (lhs->kind == GradientValue::Kind::Linear) {
        const std::vector<GradientStopValue> stops =
            blend_stops(lhs->linear.stops.data(), lhs->linear.stop_count, rhs->linear.stops.data(),
                        rhs->linear.stop_count, t);
        return format_linear(*lhs, stops, t, *rhs);
    }
    const std::vector<GradientStopValue> stops =
        blend_stops(lhs->radial.stops.data(), lhs->radial.stop_count, rhs->radial.stops.data(),
                    rhs->radial.stop_count, t);
    return format_radial(*lhs, stops, t, *rhs);
}

} // namespace tessera::ui::animation
