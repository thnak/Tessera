// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <optional>
#include <string>
#include <string_view>

// OKLCH color space for theme generation (issue #79, ADR-047 item 5). The engine's runtime color
// path stays sRGB (`color.hpp`): OKLCH exists so a generator can step lightness perceptually
// evenly and keep a seed color's hue across the accent scale, which sRGB interpolation cannot do.
// The conversion is Ottosson's OKLab transform; it is written here rather than pulled in as a
// dependency because it is a closed-form pair of 3x3 matrices (ADR-012, MIT, no new dependency).
namespace tessera::ui::style {

// A color in OKLCH: lightness 0..1, chroma 0..~0.37, hue in degrees. `hue` is meaningless when
// `chroma` is 0, so an achromatic color keeps whatever hue it was converted from.
struct OklchColor {
    double lightness = 0.0;
    double chroma = 0.0;
    double hue = 0.0;

    friend bool operator==(const OklchColor&, const OklchColor&) = default;
};

// The sRGB transfer functions of IEC 61966-2-1, the ones WCAG relative luminance also uses.
[[nodiscard]] double srgb_to_linear(double channel);
[[nodiscard]] double linear_to_srgb(double channel);

// sRGB (0..1 channels) <-> OKLCH. `to_oklch` is exact for any in-gamut input; `to_srgb` clamps
// each channel to 0..1, so an out-of-gamut request lands on the sRGB boundary instead of failing.
[[nodiscard]] OklchColor to_oklch(const ColorRgba& color);
[[nodiscard]] ColorRgba to_srgb(const OklchColor& color);

// The largest chroma that stays inside the sRGB gamut at this lightness and hue. Bisection over
// the chroma axis; the result is what a generator clamps an accent chroma to so that a step is
// not silently flattened to grey by the clamp in `to_srgb`.
[[nodiscard]] double max_chroma_in_gamut(double lightness, double hue);

// True when the color is representable without clamping each channel to 0..1.
[[nodiscard]] bool in_srgb_gamut(const OklchColor& color, double epsilon = 1e-6);

// Exactly 8-bit quantized sRGB, the only form a token value takes. The quantization happens here,
// before any contrast check, so a checker measures the color the app will actually paint.
[[nodiscard]] ColorRgba quantize_to_srgb8(const OklchColor& color);

// `#rrggbb`, lowercase. Token values in the theme asset are plain hex (stylesheet-v1.md 9.2
// requires a plain opaque color for the chrome pair, and hex is the shortest opaque form).
[[nodiscard]] std::string to_hex(const ColorRgba& color);

// Parses `#rgb`, `#rrggbb`, `rgb()` and named colors through `parse_color`, then converts.
[[nodiscard]] std::optional<OklchColor> parse_oklch(std::string_view text);

} // namespace tessera::ui::style
