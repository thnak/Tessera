// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

namespace tessera::ui::style {

struct ColorRgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct ColorParseResult {
    ColorRgba color;
    bool ok = false;
};

ColorParseResult parse_color(std::string_view text);
std::string format_color(const ColorRgba& color);
ColorRgba interpolate_color(const ColorRgba& from, const ColorRgba& to, float t);

} // namespace tessera::ui::style
