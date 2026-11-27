// SPDX-License-Identifier: MIT

#include "gradient_bodies.hpp"
#include "gradient_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tessera::ui::style::detail {
namespace {

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
// The stops (`color <position>{0,2}`, positions as a percentage or a length) go through the shared
// list parser. Returns false when the value is outside the grammar.
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

    return parse_stop_list(parts, first_stop, StopDomain::Line, out.stops, out.positions,
                           out.stop_count);
}

} // namespace tessera::ui::style::detail
