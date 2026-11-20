// SPDX-License-Identifier: MIT

// The `background-image` layer list (issue #614): parsing, printing and interpolation. Split from
// gradient.cpp (the single-gradient grammar) because the list is a different value: it owns the
// comma-separated structure, the CSS bottom-to-top order and the layer-wise interpolation, while
// gradient.cpp owns one `linear-gradient()`.

#include "tessera/ui/style/background_layers.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n\f") - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Splits at top-level commas, so the commas inside `rgba(0, 0, 0, .5)` and inside a gradient's stop
// list stay part of their layer.
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
        if (i == text.size() || (text[i] == ',' && depth == 0)) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

// The shortest arc between two CSS angles, so 350deg to 10deg blends through 0 instead of spinning
// almost a full turn. Deterministic: the same pair always picks the same direction.
float wrap_degrees(float angle) {
    while (angle > 180.0f) {
        angle -= 360.0f;
    }
    while (angle <= -180.0f) {
        angle += 360.0f;
    }
    return angle;
}

std::string format_number(float value) {
    if (std::abs(value) < 0.00005f) {
        value = 0.0f;
    }
    char buffer[64] = {};
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

// A `none` layer is the transparent image: the same gradient with every stop's alpha at 0. Pairing
// a gradient with it is what makes a layer fade in or out, and it keeps the stop count and the
// angle of the layer it pairs with so the ramp does not slide while it fades.
BackgroundLayer transparent_like(const BackgroundLayer& layer) {
    BackgroundLayer result = layer;
    if (result.kind == BackgroundLayerKind::LinearGradient) {
        for (std::size_t index = 0; index < result.gradient.stop_count; ++index) {
            result.gradient.stops[index].color.a = 0.0f;
        }
    }
    return result;
}

// One gradient layer at progress `t`. Stops pair up positionally; the shorter list repeats its last
// stop, which is the CSS rule for a stop-count mismatch (CSS Images 4 section 3.1).
LinearGradientValue blend_gradients(const LinearGradientValue& from, const LinearGradientValue& to,
                                    float t) {
    LinearGradientValue result;
    if (from.stop_count == 0 || to.stop_count == 0) {
        // A gradient layer always has 2 to 8 stops (the single-gradient grammar), so this is
        // unreachable from a parsed value; it keeps the padding arithmetic below in range anyway.
        return from.stop_count == 0 ? to : from;
    }
    result.angle_degrees = std::fmod(
        std::fmod(from.angle_degrees + wrap_degrees(to.angle_degrees - from.angle_degrees) * t,
                  360.0f) +
            360.0f,
        360.0f);
    result.stop_count = std::max(from.stop_count, to.stop_count);
    for (std::size_t index = 0; index < result.stop_count; ++index) {
        const GradientStopValue& a = from.stops[std::min(index, from.stop_count - 1)];
        const GradientStopValue& b = to.stops[std::min(index, to.stop_count - 1)];
        result.stops[index].offset = a.offset + (b.offset - a.offset) * t;
        result.stops[index].color = interpolate_color(a.color, b.color, t);
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
    result.kind = BackgroundLayerKind::LinearGradient;
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
        } else if (const std::optional<LinearGradientValue> gradient =
                       parse_linear_gradient(part)) {
            layer.kind = BackgroundLayerKind::LinearGradient;
            layer.gradient = *gradient;
        } else {
            // `url()`, a radial/conic gradient, a `repeating-*` or a malformed stop list: outside
            // this grammar, so the whole value is rejected rather than painted as a guess.
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
        out += "linear-gradient(";
        out += format_number(layer.gradient.angle_degrees);
        out += "deg";
        for (std::size_t stop = 0; stop < layer.gradient.stop_count; ++stop) {
            out += ", ";
            out += format_color(layer.gradient.stops[stop].color);
            out.push_back(' ');
            out += format_number(layer.gradient.stops[stop].offset * 100.0f);
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
