// SPDX-License-Identifier: MIT

#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

// Splits at top-level commas (a colour such as rgba(0, 0, 0, .5) has commas of its own).
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

// `<color> <percentage>?`; the colour may itself contain spaces inside its parentheses.
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

} // namespace

std::optional<LinearGradientValue> parse_linear_gradient(std::string_view text) {
    const std::string value = lower(trim(text));
    constexpr std::string_view kHead = "linear-gradient(";
    if (value.rfind(kHead, 0) != 0 || value.back() != ')') {
        return std::nullopt;
    }
    const std::string_view inner =
        std::string_view(value).substr(kHead.size(), value.size() - kHead.size() - 1);
    const std::vector<std::string_view> parts = split_commas(inner);
    if (parts.empty()) {
        return std::nullopt;
    }

    LinearGradientValue gradient;
    std::size_t first_stop = 0;
    if (const std::optional<float> angle = parse_angle(parts[0])) {
        gradient.angle_degrees = std::fmod(std::fmod(*angle, 360.0f) + 360.0f, 360.0f);
        first_stop = 1;
    } else if (const std::optional<float> direction = parse_direction(parts[0])) {
        gradient.angle_degrees = *direction;
        first_stop = 1;
    }

    const std::size_t count = parts.size() - first_stop;
    if (count < 2 || count > kMaxGradientStops) {
        return std::nullopt;
    }
    std::array<std::optional<float>, kMaxGradientStops> positions;
    for (std::size_t index = 0; index < count; ++index) {
        if (!parse_stop(parts[first_stop + index], gradient.stops[index].color, positions[index])) {
            return std::nullopt;
        }
    }
    gradient.stop_count = count;

    // CSS Images 3 section 3.4.3 colour-stop fix-up.
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
    for (std::size_t index = 0; index < count; ++index) {
        gradient.stops[index].offset = positions[index].value();
    }
    return gradient;
}

} // namespace tessera::ui::style
