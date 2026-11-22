// SPDX-License-Identifier: MIT

#include "gradient_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace tessera::ui::style::detail {

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

std::vector<std::string_view> split_spaces(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size();
        if (!end) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        const bool at_space =
            !end && depth == 0 && std::isspace(static_cast<unsigned char>(text[i])) != 0;
        if (end || at_space) {
            if (i > start) {
                parts.push_back(text.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return parts;
}

bool parse_number(const std::string& text, float& value, std::string& unit) {
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    unit = end;
    return true;
}

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
std::optional<float> parse_percentage(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit) || unit != "%") {
        return std::nullopt;
    }
    return value / 100.0f;
}

std::optional<float> parse_px_length(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit)) {
        return std::nullopt;
    }
    if (unit == "px") {
        return value;
    }
    // A bare `0` is a valid length in CSS; any other unitless number is not.
    if (unit.empty() && value == 0.0f) {
        return 0.0f;
    }
    return std::nullopt;
}

bool parse_stop(std::string_view text, ColorRgba& color, std::optional<float>& position) {
    std::string_view body = trim(text);
    position.reset();
    const std::size_t split = body.find_last_of(" \t");
    if (split != std::string_view::npos && body.back() == '%' &&
        body.find(')', split) == std::string_view::npos) {
        const std::string number(trim(body.substr(split)));
        float value = 0.0f;
        std::string unit;
        if (!parse_number(number, value, unit) || unit != "%") {
            return false;
        }
        position = value / 100.0f;
        body = trim(body.substr(0, split));
    }
    const ColorParseResult parsed = parse_color(body);
    if (!parsed.ok) {
        return false;
    }
    color = parsed.color;
    return true;
}

void fix_up_stop_offsets(std::array<std::optional<float>, kMaxGradientStops>& positions,
                         std::size_t count) {
    if (count == 0) {
        return;
    }
    if (!positions[0]) {
        positions[0] = 0.0f;
    }
    if (!positions[count - 1]) {
        positions[count - 1] = 1.0f;
    }
    float highest = positions[0].value();
    for (std::size_t index = 1; index < count; ++index) {
        if (positions[index]) {
            positions[index] = std::max(*positions[index], highest);
            highest = *positions[index];
        }
    }
    for (std::size_t index = 1; index + 1 < count; ++index) {
        if (positions[index]) {
            continue;
        }
        std::size_t next = index + 1;
        while (!positions[next]) {
            ++next;
        }
        const float from = positions[index - 1].value();
        const float step = (*positions[next] - from) / static_cast<float>(next - index + 1);
        for (std::size_t fill = index; fill < next; ++fill) {
            positions[fill] = from + step * static_cast<float>(fill - index + 1);
        }
    }
}

} // namespace tessera::ui::style::detail
