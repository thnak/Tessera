// SPDX-License-Identifier: MIT

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

struct NamedColor {
    const char* name;
    unsigned rgb;
};

constexpr NamedColor kNamedColors[] = {
    {"aliceblue", 0xf0f8ff},
    {"antiquewhite", 0xfaebd7},
    {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4},
    {"black", 0x000000},
    {"blanchedalmond", 0xffebcd},
    {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2},
    {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0},
    {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e},
    {"coral", 0xff7f50},
    {"cornflowerblue", 0x6495ed},
    {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c},
    {"cyan", 0x00ffff},
    {"darkblue", 0x00008b},
    {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9},
    {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a},
    {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969},
    {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff},
    {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700},
    {"goldenrod", 0xdaa520},
    {"gray", 0x808080},
    {"green", 0x008000},
    {"greenyellow", 0xadff2f},
    {"grey", 0x808080},
    {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4},
    {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082},
    {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c},
    {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd},
    {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0},
    {"lime", 0x00ff00},
    {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff},
    {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db},
    {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970},
    {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead},
    {"navy", 0x000080},
    {"oldlace", 0xfdf5e6},
    {"olive", 0x808000},
    {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500},
    {"orangered", 0xff4500},
    {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa},
    {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f},
    {"pink", 0xffc0cb},
    {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6},
    {"purple", 0x800080},
    {"rebeccapurple", 0x663399},
    {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0},
    {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd},
    {"slategray", 0x708090},
    {"slategrey", 0x708090},
    {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c},
    {"teal", 0x008080},
    {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee},
    {"wheat", 0xf5deb3},
    {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
};

const std::unordered_map<std::string, unsigned>& named_colors() {
    static const std::unordered_map<std::string, unsigned> colors = [] {
        std::unordered_map<std::string, unsigned> result;
        for (const NamedColor& entry : kNamedColors) {
            result.emplace(entry.name, entry.rgb);
        }
        return result;
    }();
    return colors;
}

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
    const auto named = named_colors().find(value);
    if (named != named_colors().end()) {
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
