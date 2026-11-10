// SPDX-License-Identifier: MIT

#include "color_names.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

bool parse_float(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_alpha(std::string_view text, float& alpha) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    if (token.back() == '%') {
        float percent = 0.0f;
        if (!parse_float(std::string_view(token).substr(0, token.size() - 1), percent)) {
            return false;
        }
        alpha = clamp01(percent / 100.0f);
        return true;
    }
    float number = 0.0f;
    if (!parse_float(token, number)) {
        return false;
    }
    alpha = clamp01(number);
    return true;
}

bool parse_rgb_channel(std::string_view text, float& channel) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    if (token.back() == '%') {
        float percent = 0.0f;
        if (!parse_float(std::string_view(token).substr(0, token.size() - 1), percent)) {
            return false;
        }
        channel = clamp01(percent / 100.0f);
        return true;
    }
    float number = 0.0f;
    if (!parse_float(token, number)) {
        return false;
    }
    channel = clamp01(number / 255.0f);
    return true;
}

bool parse_hue(std::string_view text, float& degrees) {
    const std::string token = lower(trim(text));
    if (token.empty()) {
        return false;
    }
    auto unit_value = [&](std::string_view suffix, float& value) {
        if (token.size() <= suffix.size() || token.substr(token.size() - suffix.size()) != suffix) {
            return false;
        }
        return parse_float(std::string_view(token).substr(0, token.size() - suffix.size()), value);
    };
    float value = 0.0f;
    if (unit_value("deg", value)) {
        degrees = value;
        return true;
    }
    if (unit_value("grad", value)) {
        degrees = value * 0.9f;
        return true;
    }
    if (unit_value("rad", value)) {
        degrees = value * 180.0f / 3.14159265358979323846f;
        return true;
    }
    if (unit_value("turn", value)) {
        degrees = value * 360.0f;
        return true;
    }
    if (parse_float(token, value)) {
        degrees = value;
        return true;
    }
    return false;
}

bool parse_hsl_component(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    if (token.back() == '%') {
        float percent = 0.0f;
        if (!parse_float(std::string_view(token).substr(0, token.size() - 1), percent)) {
            return false;
        }
        value = clamp01(percent / 100.0f);
        return true;
    }
    float number = 0.0f;
    if (!parse_float(token, number)) {
        return false;
    }
    value = clamp01(number / 100.0f);
    return true;
}

ColorRgba from_rgb(unsigned rgb) {
    return ColorRgba{static_cast<float>((rgb >> 16) & 0xffu) / 255.0f,
                     static_cast<float>((rgb >> 8) & 0xffu) / 255.0f,
                     static_cast<float>(rgb & 0xffu) / 255.0f, 1.0f};
}

bool parse_hex(std::string_view text, ColorRgba& color) {
    if (text.empty() || text.front() != '#') {
        return false;
    }
    const std::string digits = lower(text.substr(1));
    if (digits.size() != 3 && digits.size() != 4 && digits.size() != 6 && digits.size() != 8) {
        return false;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::vector<int> values;
    values.reserve(digits.size());
    for (const char c : digits) {
        const int value = nibble(c);
        if (value < 0) {
            return false;
        }
        values.push_back(value);
    }

    auto channel = [&](std::size_t index) {
        if (digits.size() <= 4) {
            return static_cast<float>(values[index] * 17) / 255.0f;
        }
        return static_cast<float>(values[index * 2] * 16 + values[index * 2 + 1]) / 255.0f;
    };
    color = ColorRgba{channel(0), channel(1), channel(2),
                      digits.size() == 4 || digits.size() == 8
                          ? static_cast<float>(values.back() * 17) / 255.0f
                          : 1.0f};
    if (digits.size() == 8) {
        color.a = static_cast<float>(values[6] * 16 + values[7]) / 255.0f;
    }
    return true;
}

std::vector<std::string> split_arguments(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ',' || c == '/' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (start != std::string_view::npos) {
                parts.push_back(trim(text.substr(start, i - start)));
                start = std::string_view::npos;
            }
        } else if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        parts.push_back(trim(text.substr(start)));
    }
    return parts;
}

ColorRgba hsl_to_rgb(float hue_degrees, float saturation, float lightness) {
    float hue = std::fmod(hue_degrees, 360.0f);
    if (hue < 0.0f) {
        hue += 360.0f;
    }
    const float c = (1.0f - std::abs(2.0f * lightness - 1.0f)) * saturation;
    const float h = hue / 60.0f;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (h < 1.0f) {
        r = c;
        g = x;
    } else if (h < 2.0f) {
        r = x;
        g = c;
    } else if (h < 3.0f) {
        g = c;
        b = x;
    } else if (h < 4.0f) {
        g = x;
        b = c;
    } else if (h < 5.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    const float m = lightness - c * 0.5f;
    return ColorRgba{r + m, g + m, b + m, 1.0f};
}

std::string format_alpha(float alpha) {
    if (std::abs(alpha - 1.0f) < 0.0005f) {
        return {};
    }
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", clamp01(alpha));
    std::string result(buffer);
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result;
}

int channel_byte(float value) {
    return static_cast<int>(std::lround(clamp01(value) * 255.0f));
}

} // namespace

ColorParseResult parse_color(std::string_view text) {
    ColorParseResult result;
    const std::string value = lower(trim(text));
    if (value.empty()) {
        return result;
    }
    if (value == "transparent") {
        result.color = ColorRgba{0.0f, 0.0f, 0.0f, 0.0f};
        result.ok = true;
        return result;
    }
    const auto named = detail::named_colors().find(value);
    if (named != detail::named_colors().end()) {
        result.color = from_rgb(named->second);
        result.ok = true;
        return result;
    }
    if (value.front() == '#') {
        result.ok = parse_hex(value, result.color);
        return result;
    }

    const std::size_t open = value.find('(');
    if (open == std::string::npos || value.back() != ')') {
        return result;
    }
    const std::string name = value.substr(0, open);
    const std::string args = value.substr(open + 1, value.size() - open - 2);
    const std::vector<std::string> parts = split_arguments(args);
    if (parts.size() != 3 && parts.size() != 4) {
        return result;
    }

    float alpha = 1.0f;
    if (parts.size() == 4 && !parse_alpha(parts[3], alpha)) {
        return result;
    }

    if (name == "rgb" || name == "rgba") {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        if (!parse_rgb_channel(parts[0], r) || !parse_rgb_channel(parts[1], g) ||
            !parse_rgb_channel(parts[2], b)) {
            return result;
        }
        result.color = ColorRgba{r, g, b, alpha};
        result.ok = true;
        return result;
    }

    if (name == "hsl" || name == "hsla") {
        float hue = 0.0f;
        float saturation = 0.0f;
        float lightness = 0.0f;
        if (!parse_hue(parts[0], hue) || !parse_hsl_component(parts[1], saturation) ||
            !parse_hsl_component(parts[2], lightness)) {
            return result;
        }
        result.color = hsl_to_rgb(hue, saturation, lightness);
        result.color.a = alpha;
        result.ok = true;
        return result;
    }

    return result;
}

std::string format_color(const ColorRgba& color) {
    const int r = channel_byte(color.r);
    const int g = channel_byte(color.g);
    const int b = channel_byte(color.b);
    const std::string alpha = format_alpha(color.a);
    if (alpha.empty()) {
        return "rgb(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) +
               ")";
    }
    return "rgba(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) +
           ", " + alpha + ")";
}

ColorRgba interpolate_color(const ColorRgba& from, const ColorRgba& to, float t) {
    const float clamped = clamp01(t);
    return ColorRgba{
        from.r + (to.r - from.r) * clamped,
        from.g + (to.g - from.g) * clamped,
        from.b + (to.b - from.b) * clamped,
        from.a + (to.a - from.a) * clamped,
    };
}

} // namespace tessera::ui::style
