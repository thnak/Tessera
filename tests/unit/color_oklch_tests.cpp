// SPDX-License-Identifier: MIT

// OKLCH color math (issue #79 task 1): sRGB <-> OKLCH against reference vectors, the sRGB transfer
// functions, gamut clamping through the maximum in-gamut chroma, and the 8-bit quantization the
// generator measures. The vectors are Björn Ottosson's published OKLab values for the sRGB
// primaries, so a matrix typo cannot pass unnoticed.

#include "tessera/ui/style/color_oklch.hpp"
#include "tessera/ui/style/contrast.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "color_oklch: " << message << "\n";
        ++failures;
    }
}

void check_near(double actual, double expected, double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "color_oklch: " << message << ": expected " << expected << ", got " << actual
                  << "\n";
        ++failures;
    }
}

style::ColorRgba from_bytes(int r, int g, int b) {
    return style::ColorRgba{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                            static_cast<float>(b) / 255.0f, 1.0f};
}

} // namespace

int main() {
    // The sRGB transfer functions are inverses over the unit range, and the piecewise breakpoint is
    // the standard 0.04045 / 0.0031308 pair.
    check_near(style::srgb_to_linear(0.0), 0.0, 1e-12, "linear black");
    check_near(style::srgb_to_linear(1.0), 1.0, 1e-12, "linear white");
    check_near(style::linear_to_srgb(0.0), 0.0, 1e-12, "srgb black");
    check_near(style::linear_to_srgb(1.0), 1.0, 1e-12, "srgb white");
    for (int i = 0; i <= 255; ++i) {
        const double value = static_cast<double>(i) / 255.0;
        check_near(style::linear_to_srgb(style::srgb_to_linear(value)), value, 1e-9,
                   "transfer round trip at " + std::to_string(i));
    }

    // Reference vectors: the sRGB primaries in OKLCH (Ottosson's OKLab values, hue in degrees).
    struct Reference {
        int r;
        int g;
        int b;
        double lightness;
        double chroma;
        double hue;
    };
    // Tolerances are 1e-4 because the published constants are quoted to 10 decimal places.
    const Reference references[] = {
        {255, 0, 0, 0.627955, 0.257683, 29.2339},
        {0, 255, 0, 0.866440, 0.294827, 142.4953},
        {0, 0, 255, 0.452014, 0.313214, 264.0520},
        {255, 255, 255, 1.0, 0.0, 0.0},
        {0, 0, 0, 0.0, 0.0, 0.0},
    };
    for (const Reference& reference : references) {
        const style::OklchColor converted =
            style::to_oklch(from_bytes(reference.r, reference.g, reference.b));
        const std::string label = "srgb(" + std::to_string(reference.r) + ", " +
                                  std::to_string(reference.g) + ", " + std::to_string(reference.b) +
                                  ")";
        check_near(converted.lightness, reference.lightness, 1e-4, label + " lightness");
        check_near(converted.chroma, reference.chroma, 1e-4, label + " chroma");
        if (reference.chroma > 0.01) {
            check_near(converted.hue, reference.hue, 0.02, label + " hue");
        }
    }

    // An achromatic color has no hue to carry, so only its lightness and chroma are checked.
    const style::OklchColor grey = style::to_oklch(from_bytes(128, 128, 128));
    check_near(grey.chroma, 0.0, 1e-6, "grey has no chroma");
    check_near(grey.lightness, 0.599871, 1e-4, "grey lightness");

    // sRGB -> OKLCH -> sRGB round trips exactly on the 8-bit grid.
    for (int r = 0; r <= 255; r += 17) {
        for (int g = 0; g <= 255; g += 17) {
            for (int b = 0; b <= 255; b += 17) {
                const style::ColorRgba original = from_bytes(r, g, b);
                const style::ColorRgba back = style::quantize_to_srgb8(style::to_oklch(original));
                check(std::lround(back.r * 255.0f) == r && std::lround(back.g * 255.0f) == g &&
                          std::lround(back.b * 255.0f) == b,
                      "round trip of srgb(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
                          std::to_string(b) + ")");
            }
        }
    }

    // The gamut: an achromatic lightness has no room for chroma at the extremes, so every request
    // there lands on black or white, and the boundary is not monotone in lightness at a fixed hue.
    check_near(style::max_chroma_in_gamut(1.0, 254.0), 0.0, 1e-3, "white max chroma");
    // At L 0 the linear channels are all ~0, so the bisection legitimately finds a sliver of chroma
    // that still rounds to black; what matters is that the quantized color is black.
    check(style::to_hex(style::quantize_to_srgb8(
              style::OklchColor{0.0, style::max_chroma_in_gamut(0.0, 254.0), 254.0})) == "#000000",
          "an L 0 color quantizes to black whatever its chroma");
    const double mid_blue = style::max_chroma_in_gamut(0.5, 264.0);
    check(mid_blue > 0.2 && mid_blue < 0.3, "mid blue max chroma is about 0.28");
    // A request beyond the gamut is clamped, not wrapped: the hue survives the trip.
    style::OklchColor out_of_gamut;
    out_of_gamut.lightness = 0.5;
    out_of_gamut.chroma = 0.5;
    out_of_gamut.hue = 264.0;
    check(!style::in_srgb_gamut(out_of_gamut), "chroma 0.5 at L 0.5 is out of gamut");
    const style::ColorRgba clamped = style::to_srgb(out_of_gamut);
    check(clamped.r >= 0.0f && clamped.r <= 1.0f && clamped.g >= 0.0f && clamped.g <= 1.0f &&
              clamped.b >= 0.0f && clamped.b <= 1.0f,
          "out-of-gamut color clamps into 0..1");
    // Clamping the chroma first keeps the hue; clamping the channels after would not.
    style::OklchColor in_gamut = out_of_gamut;
    in_gamut.chroma = style::max_chroma_in_gamut(0.5, 264.0);
    check(style::in_srgb_gamut(in_gamut), "the maximum in-gamut chroma is in gamut");
    const style::OklchColor back = style::to_oklch(style::to_srgb(in_gamut));
    check_near(back.hue, 264.0, 0.5, "clamping chroma keeps the hue");
    check_near(back.chroma, in_gamut.chroma, 1e-3, "clamped chroma survives");

    // Hex formatting is lowercase `#rrggbb`, the token value form.
    check(style::to_hex(from_bytes(0, 0, 0)) == "#000000", "hex black");
    check(style::to_hex(from_bytes(255, 255, 255)) == "#ffffff", "hex white");
    check(style::to_hex(from_bytes(11, 92, 173)) == "#0b5cad", "hex seed");

    // parse_oklch accepts what parse_color accepts and rejects what it rejects.
    const auto parsed_seed = style::parse_oklch("#0b5cad");
    check(parsed_seed.has_value(), "parses a six-digit hex seed");
    if (parsed_seed) {
        check_near(parsed_seed->lightness, 0.477822, 1e-4, "seed lightness");
        check_near(parsed_seed->chroma, 0.147444, 1e-4, "seed chroma");
        check_near(parsed_seed->hue, 254.3108, 0.02, "seed hue");
    }
    check(style::parse_oklch("#0b5").has_value(), "a three-digit hex shorthand parses");
    check(style::to_hex(style::quantize_to_srgb8(*style::parse_oklch("#0b5"))) == "#00bb55",
          "the #0b5 shorthand expands to #00bb55");
    check(style::parse_oklch("rgb(11, 92, 173)").has_value(), "parses rgb()");
    check(style::parse_oklch("not-a-color").has_value() == false, "rejects a non-color");
    check(style::to_hex(style::quantize_to_srgb8(*style::parse_oklch("#0B5CAD"))) == "#0b5cad",
          "hex round trip through OKLCH");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::printf("color_oklch: ok\n");
    return 0;
}
