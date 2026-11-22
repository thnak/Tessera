// SPDX-License-Identifier: MIT

#include "shadow_interpolation.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace tessera::ui::animation {

using tessera::detail::lower;
using tessera::detail::split_top_level;
using tessera::detail::trim;
namespace {

// Matches paint_shadow.hpp's kMaxBoxShadows: the paint pass honours no more layers than this, so a
// longer list is not worth interpolating past it.
constexpr std::size_t kMaxLayers = 4;

struct Layer {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float blur = 0.0f;
    float spread = 0.0f;
    style::ColorRgba color{0.0f, 0.0f, 0.0f, 0.0f};
    bool inset = false;
};

// Splits at `separator` outside parentheses (a colour such as rgba(0, 0, 0, .2) has commas and
// spaces of its own).

bool is_length_token(std::string_view token) {
    const char c = token.front();
    return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '+' || c == '.';
}

// A length in px, or a bare 0. Any other unit is outside what this interpolator resolves.
bool parse_px(std::string_view text, float& value) {
    std::string token = lower(text);
    bool has_unit = false;
    if (token.size() > 2 && token.compare(token.size() - 2, 2, "px") == 0) {
        token.resize(token.size() - 2);
        has_unit = true;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (token.empty() || end == nullptr || *end != '\0' || !std::isfinite(parsed) ||
        (!has_unit && parsed != 0.0f)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_layer(std::string_view text, Layer& out, bool text_shadow) {
    float lengths[4] = {};
    int length_count = 0;
    bool have_color = false;
    out = Layer{};
    for (const std::string_view token : split_top_level(text, ' ')) {
        const std::string word = lower(token);
        if (word == "inset") {
            // `text-shadow` has no inset form, so the token makes the value a `box-shadow` one.
            if (text_shadow) {
                return false;
            }
            out.inset = true;
        } else if (is_length_token(token)) {
            if (length_count == 4 || !parse_px(token, lengths[length_count])) {
                return false;
            }
            ++length_count;
        } else {
            // `currentColor` resolves against the node's own `color`, which this string-level
            // sampler cannot see; leaving it to the discrete switch is the honest answer.
            const style::ColorParseResult parsed = style::parse_color(token);
            if (!parsed.ok || have_color) {
                return false;
            }
            have_color = true;
            out.color = parsed.color;
        }
    }
    if (length_count < 2 || !have_color) {
        return false;
    }
    // `text-shadow` is `[<color>? <x> <y> <blur>?]`: a fourth length is a spread, which it has no
    // grammar for.
    if (text_shadow && length_count > 3) {
        return false;
    }
    out.offset_x = lengths[0];
    out.offset_y = lengths[1];
    out.blur = length_count >= 3 ? lengths[2] : 0.0f;
    out.spread = length_count >= 4 ? lengths[3] : 0.0f;
    return out.blur >= 0.0f;
}

bool parse_list(std::string_view text, std::vector<Layer>& out, bool text_shadow) {
    out.clear();
    const std::string_view value = trim(text);
    if (value.empty()) {
        return false;
    }
    if (lower(value) == "none") {
        return true;
    }
    for (const std::string_view part : split_top_level(value, ',')) {
        Layer layer;
        if (out.size() == kMaxLayers || !parse_layer(trim(part), layer, text_shadow)) {
            return false;
        }
        out.push_back(layer);
    }
    return !out.empty();
}

float lerp(float from, float to, double t) {
    return static_cast<float>(from + (to - from) * t);
}

std::string format_length(float value) {
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
    return (result.empty() ? "0" : result) + "px";
}

std::string format_layer(const Layer& layer, bool text_shadow) {
    std::string text = layer.inset ? "inset " : "";
    text += format_length(layer.offset_x) + " " + format_length(layer.offset_y) + " " +
            format_length(layer.blur);
    // `text-shadow` has no spread, so emitting one would produce a value its own parser rejects.
    if (!text_shadow) {
        text += " " + format_length(layer.spread);
    }
    text += " " + style::format_color(layer.color);
    return text;
}

// The shared layer walk. `text_shadow` pins the two fields its grammar lacks (spread, inset) and
// formats without them, so one implementation serves both properties.
std::optional<std::string> interpolate_shadow_list(std::string_view from, std::string_view to,
                                                   double t, bool text_shadow) {
    std::vector<Layer> lhs;
    std::vector<Layer> rhs;
    if (!parse_list(from, lhs, text_shadow) || !parse_list(to, rhs, text_shadow) ||
        !std::isfinite(t)) {
        return std::nullopt;
    }
    const std::size_t count = std::max(lhs.size(), rhs.size());
    if (count == 0) {
        return std::string("none"); // none -> none
    }
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        // A missing layer is a transparent zero shadow that borrows its partner's `inset` flag.
        Layer a;
        Layer b;
        if (index < lhs.size()) {
            a = lhs[index];
            b.inset = a.inset;
        }
        if (index < rhs.size()) {
            b = rhs[index];
            if (index >= lhs.size()) {
                a.inset = b.inset;
            }
        }
        if (a.inset != b.inset) {
            return std::nullopt;
        }
        Layer layer;
        layer.inset = a.inset;
        layer.offset_x = lerp(a.offset_x, b.offset_x, t);
        layer.offset_y = lerp(a.offset_y, b.offset_y, t);
        layer.blur = std::max(0.0f, lerp(a.blur, b.blur, t));
        layer.spread = text_shadow ? 0.0f : lerp(a.spread, b.spread, t);
        layer.color = style::interpolate_color(a.color, b.color, static_cast<float>(t));
        if (!result.empty()) {
            result += ", ";
        }
        result += format_layer(layer, text_shadow);
    }
    return result;
}

} // namespace

std::optional<std::string> interpolate_box_shadow(std::string_view from, std::string_view to,
                                                  double t) {
    return interpolate_shadow_list(from, to, t, false);
}

std::optional<std::string> interpolate_text_shadow(std::string_view from, std::string_view to,
                                                   double t) {
    return interpolate_shadow_list(from, to, t, true);
}

} // namespace tessera::ui::animation
