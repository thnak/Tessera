// SPDX-License-Identifier: MIT

#include "paint_properties.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::ui::paint::detail {
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

bool parse_number(std::string_view text, float& value) {
    const std::string token(trim(text));
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

const std::string* raw(const style::ComputedStyle& style, const char* property) {
    return style.find(property);
}

std::optional<rhi::Color> from_style_color(const style::ColorRgba& color) {
    return rhi::Color{color.r, color.g, color.b, color.a};
}

} // namespace

std::optional<float> length_value(const style::ComputedStyle& style, const char* property) {
    const std::string* text = raw(style, property);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string_view token = trim(*text);
    if (token.empty()) {
        return std::nullopt;
    }
    if (token.size() > 2 && token.substr(token.size() - 2) == "px") {
        token = trim(token.substr(0, token.size() - 2));
    } else if (std::isalpha(static_cast<unsigned char>(token.back())) != 0 || token.back() == '%') {
        return std::nullopt;
    }

    float value = 0.0f;
    if (!parse_number(token, value) || value < 0.0f) {
        return std::nullopt;
    }
    return value;
}

float opacity_value(const style::ComputedStyle& style) {
    const std::string* text = raw(style, "opacity");
    if (text == nullptr) {
        return 1.0f;
    }
    float value = 1.0f;
    if (!parse_number(*text, value)) {
        return 1.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

std::optional<rhi::Color> color_value(const style::ComputedStyle& style, const char* property,
                                      const rhi::Color& current_color) {
    const std::string* text = raw(style, property);
    if (text == nullptr) {
        return std::nullopt;
    }
    if (lower(trim(*text)) == "currentcolor") {
        return current_color;
    }
    const style::ColorParseResult parsed = style::parse_color(*text);
    if (!parsed.ok) {
        return std::nullopt;
    }
    return from_style_color(parsed.color);
}

rhi::Color current_color(const style::ComputedStyle& style) {
    const std::string* text = raw(style, "color");
    if (text != nullptr) {
        const style::ColorParseResult parsed = style::parse_color(*text);
        if (parsed.ok) {
            return from_style_color(parsed.color).value();
        }
    }
    // CSS drops an invalid declaration, so the inherited/initial value (black) applies.
    return rhi::Color{0.0f, 0.0f, 0.0f, 1.0f};
}

float border_width(const style::ComputedStyle& style, const char* width_property,
                   const char* style_property) {
    const std::string* style_text = raw(style, style_property);
    if (style_text == nullptr) {
        return 0.0f;
    }
    const std::string keyword = lower(trim(*style_text));
    if (keyword.empty() || keyword == "none" || keyword == "hidden") {
        return 0.0f;
    }
    const std::optional<float> width = length_value(style, width_property);
    return width.value_or(0.0f);
}

BorderWidths border_widths(const style::ComputedStyle& style, const rhi::Rect& rect) {
    BorderWidths widths;
    widths.top = border_width(style, "border-top-width", "border-top-style");
    widths.right = border_width(style, "border-right-width", "border-right-style");
    widths.bottom = border_width(style, "border-bottom-width", "border-bottom-style");
    widths.left = border_width(style, "border-left-width", "border-left-style");

    widths.top = std::min(widths.top, rect.height);
    widths.bottom = std::min(widths.bottom, rect.height - widths.top);
    widths.left = std::min(widths.left, rect.width);
    widths.right = std::min(widths.right, rect.width - widths.left);
    return widths;
}

std::optional<rhi::Rect> padding_box(const style::ComputedStyle& style, const rhi::Rect& rect) {
    const BorderWidths widths = border_widths(style, rect);
    const float width = rect.width - widths.left - widths.right;
    const float height = rect.height - widths.top - widths.bottom;
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return rhi::Rect{rect.x + widths.left, rect.y + widths.top, width, height};
}

std::optional<float> uniform_radius(const style::ComputedStyle& style) {
    static constexpr const char* kRadiusProperties[4] = {
        "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
        "border-bottom-left-radius"};
    float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int index = 0; index < 4; ++index) {
        if (raw(style, kRadiusProperties[index]) == nullptr) {
            continue;
        }
        const std::optional<float> parsed = length_value(style, kRadiusProperties[index]);
        if (!parsed) {
            return std::nullopt;
        }
        radii[index] = *parsed;
    }
    if (radii[0] != radii[1] || radii[0] != radii[2] || radii[0] != radii[3]) {
        return std::nullopt;
    }
    return radii[0];
}

bool clips_children(const style::ComputedStyle& style) {
    static constexpr const char* kOverflowProperties[2] = {"overflow-x", "overflow-y"};
    for (const char* property : kOverflowProperties) {
        const std::string* text = raw(style, property);
        if (text == nullptr) {
            continue;
        }
        const std::string keyword = lower(trim(*text));
        if (!keyword.empty() && keyword != "visible") {
            // hidden/clip/scroll/auto all clip: the paint pass has no scrolling, so the visible
            // child region is the border box for every non-visible overflow value.
            return true;
        }
    }
    return false;
}

RoundedClip corner_clip(const style::ComputedStyle& style, const rhi::Rect& rect) {
    static constexpr const char* kRadiusProperties[4] = {
        "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
        "border-bottom-left-radius"};
    RoundedClip clip;
    clip.rect = rect;
    float* destinations[4] = {&clip.top_left, &clip.top_right, &clip.bottom_right,
                              &clip.bottom_left};
    const float limit = std::min(rect.width, rect.height) * 0.5f;
    for (int index = 0; index < 4; ++index) {
        const std::optional<float> parsed = length_value(style, kRadiusProperties[index]);
        const float radius = parsed.value_or(0.0f);
        *destinations[index] = std::clamp(radius, 0.0f, std::max(limit, 0.0f));
    }
    return clip;
}

float font_size_value(const style::ComputedStyle& style) {
    const std::optional<float> size = length_value(style, "font-size");
    return size.value_or(16.0f);
}

int font_weight_value(const style::ComputedStyle& style) {
    const std::string* text = raw(style, "font-weight");
    if (text == nullptr) {
        return 400;
    }
    float weight = 0.0f;
    if (!parse_number(*text, weight)) {
        return 400;
    }
    return std::clamp(static_cast<int>(std::lround(weight)), 1, 1000);
}

std::optional<style::BackdropFilter> backdrop_filter_value(const style::ComputedStyle& style) {
    static constexpr const char* kProperties[2] = {"backdrop-filter", "-webkit-backdrop-filter"};
    for (const char* property : kProperties) {
        const std::string* text = raw(style, property);
        if (text == nullptr) {
            continue;
        }
        const style::BackdropFilterParseResult parsed = style::parse_backdrop_filter(*text);
        // The registry drops an unsupported declaration before the cascade, so an unparseable
        // computed value can only come from a hand-built style; paint treats it as `none`.
        if (!parsed.ok() || parsed.filter.none()) {
            continue;
        }
        return parsed.filter;
    }
    return std::nullopt;
}

} // namespace tessera::ui::paint::detail
