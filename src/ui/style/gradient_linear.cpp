// SPDX-License-Identifier: MIT

#include "gradient_bodies.hpp"
#include "gradient_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tessera::ui::style::detail {
namespace {

std::optional<float> parse_angle(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit)) {
        return std::nullopt;
    }
    if (unit == "deg") {
        return value;
    }
    if (unit == "grad") {
        return value * 0.9f;
    }
    if (unit == "rad") {
        return value * 180.0f / 3.14159265358979f;
    }
    if (unit == "turn") {
        return value * 360.0f;
    }
    return value == 0.0f && unit.empty() ? std::optional<float>(0.0f) : std::nullopt;
}

std::optional<float> parse_direction(std::string_view text) {
    const std::string token = lower(trim(text));
    if (token.rfind("to ", 0) != 0) {
        return std::nullopt;
    }
    bool top = false;
    bool bottom = false;
    bool left = false;
    bool right = false;
    std::size_t start = 3;
    while (start < token.size()) {
        const std::size_t end = std::min(token.find(' ', start), token.size());
        const std::string word = token.substr(start, end - start);
        if (word == "top") {
            top = true;
        } else if (word == "bottom") {
            bottom = true;
        } else if (word == "left") {
            left = true;
        } else if (word == "right") {
            right = true;
        } else if (!word.empty()) {
            return std::nullopt;
        }
        start = end + 1;
    }
    if ((top && bottom) || (left && right) || !(top || bottom || left || right)) {
        return std::nullopt;
    }
    if (top && right) {
        return 45.0f;
    }
    if (bottom && right) {
        return 135.0f;
    }
    if (bottom && left) {
        return 225.0f;
    }
    if (top && left) {
        return 315.0f;
    }
    return top ? 0.0f : right ? 90.0f : bottom ? 180.0f : 270.0f;
}

} // namespace

// Parses the inside of `linear-gradient(...)` (the text between the parentheses) into `out`.
// `out.stops[i].offset` carries the authored position, or NaN when the stop had none; the caller
// runs the shared fix-up. Returns false when the value is outside the grammar.
bool parse_linear_body(std::string_view inner, LinearGradientValue& out) {
    const std::vector<std::string_view> parts = split_commas(inner);
    if (parts.empty()) {
        return false;
    }

    std::size_t first_stop = 0;
    if (const std::optional<float> angle = parse_angle(parts[0])) {
        out.angle_degrees = std::fmod(std::fmod(*angle, 360.0f) + 360.0f, 360.0f);
        first_stop = 1;
    } else if (const std::optional<float> direction = parse_direction(parts[0])) {
        out.angle_degrees = *direction;
        first_stop = 1;
    }

    const std::size_t count = parts.size() - first_stop;
    if (count < 2 || count > kMaxGradientStops) {
        return false;
    }
    std::array<std::optional<float>, kMaxGradientStops> positions;
    for (std::size_t index = 0; index < count; ++index) {
        if (!parse_stop(parts[first_stop + index], out.stops[index].color, positions[index])) {
            return false;
        }
    }
    out.stop_count = count;
    fix_up_stop_offsets(positions, count);
    for (std::size_t index = 0; index < count; ++index) {
        out.stops[index].offset = positions[index].value();
    }
    return true;
}

} // namespace tessera::ui::style::detail
