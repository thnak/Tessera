// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Seams shared by the halves of theme generation: the OKLCH derivation in `theme_generate.cpp` and
// the Instrument design tables in `theme_generate_tokens.cpp`. Private to `src/ui/style`.
namespace tessera::ui::style::detail {

// The tuning constants of the derivation, in one place so the algorithm file is only the algorithm.
struct InstrumentTuning {
    // Every solved pair clears its floor by this much, so an 8-bit rounding on either side of a
    // comparison cannot turn a pass into a fail.
    double contrast_margin = 1.012;
    // The lightness step of the solve search. 0.0025 is finer than one 8-bit sRGB step at every
    // lightness a role can take, so the solve cannot skip over a passing value.
    double solve_step = 0.0025;
    // A seed below this chroma has no usable hue; the accent falls back to the Instrument blue.
    double achromatic_threshold = 0.004;
    double instrument_hue = 254.0;
    double accent_chroma_min = 0.060;
    double accent_chroma_max = 0.170;
    // The chroma profile of the accent scale: full at L 0.62, tapering to 15 % at the ends so the
    // pale and dark steps do not clip against the sRGB boundary and go grey.
    double accent_chroma_peak = 0.62;
    double accent_chroma_taper = 1.9;
    double accent_chroma_floor = 0.15;
};

[[nodiscard]] const InstrumentTuning& instrument_tuning();

// The status colors: their conventional hue and chroma, held regardless of the seed because a red
// that is not red is a worse failure than a theme that does not tint its errors.
struct SemanticHue {
    std::string_view role;
    double hue;
    double chroma;
};

[[nodiscard]] std::span<const SemanticHue> instrument_semantic_hues();

// The accent tonal scale the generator emits and solves against.
[[nodiscard]] std::span<const AccentScaleStep> instrument_scale_steps();

// Appends the non-color tokens of the Instrument design language (spacing on the 4 px grid, radii
// in the 2 to 8 px range, typography, motion, breakpoints, z-layers, control metrics and the
// floating shadow) to `theme`. Only `--border-width` and `--shadow-floating-color` differ between
// the variants; every other static token is identical in all three (ADR-047 item 5).
void append_static_tokens(Theme& theme, const ThemeVariantSpec& spec);

// ---------------------------------------------------------------------------------------------
// The OKLCH solve, in theme_solve.cpp. It is separate from the derivation in theme_generate.cpp
// because it is the part that talks to contrast.hpp and the part a reviewer most needs to read on
// its own.
// ---------------------------------------------------------------------------------------------

// One variant's three surfaces plus the two floors the solve targets, captured once per variant.
struct SolveSurfaces {
    std::array<ColorRgba, 3> colors{};
    double text_floor = 4.5;
    double indicator_floor = 3.0;
};

// The surfaces, floors and ink of one variant, which every solve below is measured against.
SolveSurfaces solve_surfaces(const ThemeVariantSpec& spec, double hue);

// An OKLCH lightness/hue/chroma request, quantized to the 8-bit sRGB the app will paint. The chroma
// is clamped to the gamut first so `to_srgb` never has to clamp a channel, which would shift the
// hue away from the seed's.
[[nodiscard]] ColorRgba color_at(double lightness, double chroma, double hue);

// The chroma of one accent-scale step: the accent chroma scaled by the taper profile.
[[nodiscard]] double chroma_of_step(double lightness, double accent_chroma);

// Walks outward from `target_lightness`, nearest first, and returns the first color that passes
// `accept`, or nullopt when the whole lightness range fails.
template<typename Accept>
[[nodiscard]] std::optional<ColorRgba> solve_lightness(double target_lightness, double chroma,
                                                       double hue, Accept accept) {
    constexpr int kReach = 400; // 400 * 0.0025 covers the whole 0..1 range from any target
    const double step_size = instrument_tuning().solve_step;
    for (int step = 0; step <= kReach; ++step) {
        const double offset = static_cast<double>(step) * step_size;
        const std::array<double, 2> candidates{target_lightness - offset,
                                               target_lightness + offset};
        for (const double lightness : candidates) {
            if (lightness < 0.0 || lightness > 1.0) {
                continue;
            }
            const ColorRgba candidate = color_at(lightness, chroma, hue);
            if (accept(candidate)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

// True when `color` clears `floor` against every surface.
[[nodiscard]] bool clears_surfaces(const ColorRgba& color, const SolveSurfaces& surfaces,
                                   double floor);

// The scale step nearest `target_lightness` whose index passes `accept`; ties go to the darker step
// so a generated scale is stable and reads as "the quiet side of a threshold".
template<typename Accept>
[[nodiscard]] std::optional<std::size_t> nearest_scale_step(double target_lightness,
                                                            Accept accept) {
    const std::span<const AccentScaleStep> scale = instrument_scale_steps();
    std::vector<std::size_t> order(scale.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [target_lightness, scale](std::size_t left, std::size_t right) {
                  const double left_distance = std::abs(scale[left].lightness - target_lightness);
                  const double right_distance = std::abs(scale[right].lightness - target_lightness);
                  if (left_distance != right_distance) {
                      return left_distance < right_distance;
                  }
                  return scale[left].lightness < scale[right].lightness;
              });
    for (const std::size_t index : order) {
        if (accept(index)) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace tessera::ui::style::detail
