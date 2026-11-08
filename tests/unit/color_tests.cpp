// SPDX-License-Identifier: MIT

#include "tessera/ui/style/color.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::ColorRgba;
using tessera::ui::style::format_color;
using tessera::ui::style::interpolate_color;
using tessera::ui::style::parse_color;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "color: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.005f;
}

bool color_is(const ColorRgba& color, float r, float g, float b, float a) {
    return near(color.r, r) && near(color.g, g) && near(color.b, b) && near(color.a, a);
}

ColorRgba parsed(std::string_view text) {
    const auto result = parse_color(text);
    if (!result.ok) {
        std::cerr << "color: failed to parse " << text << "\n";
    }
    return result.color;
}

} // namespace

int main() {
    if (!expect(color_is(parsed("red"), 1, 0, 0, 1), "named red") ||
        !expect(color_is(parsed("rebeccapurple"), 0.4f, 0.2f, 0.6f, 1), "named rare") ||
        !expect(color_is(parsed("transparent"), 0, 0, 0, 0), "transparent") ||
        !expect(!parse_color("definitely-not-a-color").ok, "unknown name")) {
        return 1;
    }

    if (!expect(color_is(parsed("#f00"), 1, 0, 0, 1), "short hex") ||
        !expect(color_is(parsed("#ff000080"), 1, 0, 0, 128.0f / 255.0f), "hex alpha") ||
        !expect(color_is(parsed("#abcd"), 170.0f / 255.0f, 187.0f / 255.0f, 205.0f / 255.0f,
                         221.0f / 255.0f),
                "short hex alpha") ||
        !expect(!parse_color("#12").ok, "bad hex length")) {
        return 2;
    }

    if (!expect(color_is(parsed("rgb(255, 0, 0)"), 1, 0, 0, 1), "rgb numbers") ||
        !expect(color_is(parsed("rgb(100%, 0%, 50%)"), 1, 0, 0.5f, 1), "rgb percent") ||
        !expect(color_is(parsed("rgba(255, 0, 0, 0.5)"), 1, 0, 0, 0.5f), "rgba alpha") ||
        !expect(color_is(parsed("rgb(255 0 0 / 50%)"), 1, 0, 0, 0.5f), "modern rgb") ||
        !expect(!parse_color("rgb(1, 2)").ok, "rgb arity")) {
        return 3;
    }

    if (!expect(color_is(parsed("hsl(120, 100%, 50%)"), 0, 1, 0, 1), "hsl green") ||
        !expect(color_is(parsed("hsl(240 100% 50% / 0.5)"), 0, 0, 1, 0.5f), "hsl alpha") ||
        !expect(color_is(parsed("hsl(0.5turn, 100%, 50%)"), 0, 1, 1, 1), "hsl turn") ||
        !expect(!parse_color("hsl(0, 100%)").ok, "hsl arity")) {
        return 4;
    }

    if (!expect(format_color(ColorRgba{1, 0, 0, 1}) == "rgb(255, 0, 0)", "format rgb") ||
        !expect(format_color(ColorRgba{1, 0, 0, 0.5f}) == "rgba(255, 0, 0, 0.5)", "format rgba")) {
        return 5;
    }

    const ColorRgba mixed = interpolate_color(parsed("red"), parsed("blue"), 0.5f);
    if (!expect(color_is(mixed, 0.5f, 0, 0.5f, 1), "interpolation channels") ||
        !expect(format_color(mixed) == "rgb(128, 0, 128)", "interpolation format")) {
        return 6;
    }

    std::cout << "color: ok\n";
    return 0;
}
