// SPDX-License-Identifier: MIT

// OKLCH conversion for theme generation (issue #79). The matrices are Björn Ottosson's published
// OKLab transforms (MIT); see docs/specs/theme-generation-v1.md for the reference vectors the unit
// test pins them against.

#include "tessera/ui/style/color_oklch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tessera::ui::style {
namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

// OKLab <-> linear sRGB, the two 3x3 matrix pairs of the OKLab definition.
void linear_srgb_to_oklab(double r, double g, double b, double& l, double& m, double& s) {
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
}

void oklab_to_linear_srgb(double big_l, double a, double big_b, double& r, double& g, double& b) {
    const double l_ = big_l + 0.3963377774 * a + 0.2158037573 * big_b;
    const double m_ = big_l - 0.1055613458 * a - 0.0638541728 * big_b;
    const double s_ = big_l - 0.0894841775 * a - 1.2914855480 * big_b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

int channel_byte(double channel) {
    return static_cast<int>(std::lround(clamp01(channel) * 255.0));
}

} // namespace

double srgb_to_linear(double channel) {
    const double value = clamp01(channel);
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double channel) {
    const double value = clamp01(channel);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

OklchColor to_oklch(const ColorRgba& color) {
    double l = 0.0;
    double m = 0.0;
    double s = 0.0;
    linear_srgb_to_oklab(srgb_to_linear(color.r), srgb_to_linear(color.g), srgb_to_linear(color.b),
                         l, m, s);
    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);
    const double lightness = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    const double a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    const double b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
    OklchColor result;
    result.lightness = lightness;
    result.chroma = std::sqrt(a * a + b * b);
    result.hue = std::atan2(b, a) * 180.0 / 3.14159265358979323846;
    if (result.hue < 0.0) {
        result.hue += 360.0;
    }
    return result;
}

ColorRgba to_srgb(const OklchColor& color) {
    const double radians = color.hue * 3.14159265358979323846 / 180.0;
    const double a = color.chroma * std::cos(radians);
    const double b = color.chroma * std::sin(radians);
    double r = 0.0;
    double g = 0.0;
    double blue = 0.0;
    oklab_to_linear_srgb(color.lightness, a, b, r, g, blue);
    return ColorRgba{static_cast<float>(linear_to_srgb(r)), static_cast<float>(linear_to_srgb(g)),
                     static_cast<float>(linear_to_srgb(blue)), 1.0f};
}

bool in_srgb_gamut(const OklchColor& color, double epsilon) {
    const double radians = color.hue * 3.14159265358979323846 / 180.0;
    const double a = color.chroma * std::cos(radians);
    const double b = color.chroma * std::sin(radians);
    double r = 0.0;
    double g = 0.0;
    double blue = 0.0;
    oklab_to_linear_srgb(color.lightness, a, b, r, g, blue);
    for (const double channel : {r, g, blue}) {
        if (channel < -epsilon || channel > 1.0 + epsilon) {
            return false;
        }
    }
    return true;
}

double max_chroma_in_gamut(double lightness, double hue) {
    OklchColor probe;
    probe.lightness = lightness;
    probe.hue = hue;
    if (!in_srgb_gamut(probe)) {
        return 0.0;
    }
    // Bisection: `low` is always in gamut and `high` always out, so 32 rounds pin the boundary to
    // well under one 8-bit step at every lightness.
    double low = 0.0;
    double high = 0.5;
    for (int i = 0; i < 32; ++i) {
        const double mid = (low + high) * 0.5;
        probe.chroma = mid;
        if (in_srgb_gamut(probe)) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

ColorRgba quantize_to_srgb8(const OklchColor& color) {
    const ColorRgba raw = to_srgb(color);
    return ColorRgba{static_cast<float>(channel_byte(raw.r)) / 255.0f,
                     static_cast<float>(channel_byte(raw.g)) / 255.0f,
                     static_cast<float>(channel_byte(raw.b)) / 255.0f, 1.0f};
}

std::string to_hex(const ColorRgba& color) {
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", channel_byte(color.r),
                  channel_byte(color.g), channel_byte(color.b));
    return std::string(buffer);
}

std::optional<OklchColor> parse_oklch(std::string_view text) {
    const ColorParseResult parsed = parse_color(text);
    if (!parsed.ok) {
        return std::nullopt;
    }
    return to_oklch(parsed.color);
}

} // namespace tessera::ui::style
