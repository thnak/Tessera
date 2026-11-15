// SPDX-License-Identifier: MIT

// The OKLCH solve of theme generation (issue #79): the surfaces a variant is measured against, and
// the two searches that place a role. It is split from the derivation in theme_generate.cpp so the
// part that talks to contrast.hpp reads on its own.
//
// The rule in one sentence: walk OKLCH lightness outward from the role's design target in 0.0025
// steps, nearest first, and take the first color that clears its WCAG floor by the tuning margin.

#include "tessera/ui/style/color_oklch.hpp"
#include "tessera/ui/style/contrast.hpp"
#include "theme_generate_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tessera::ui::style::detail {

SolveSurfaces solve_surfaces(const ThemeVariantSpec& spec, double hue) {
    const InstrumentTuning& tuning = instrument_tuning();
    SolveSurfaces surfaces;
    surfaces.colors = {color_at(spec.surface, spec.neutral_chroma, hue),
                       color_at(spec.surface_raised, spec.neutral_chroma, hue),
                       color_at(spec.surface_sunken, spec.neutral_chroma, hue)};
    surfaces.text_floor = contrast_floor(ContrastRequirement::NormalText, spec.high_contrast) *
                          tuning.contrast_margin;
    surfaces.indicator_floor =
        contrast_floor(ContrastRequirement::NonText, spec.high_contrast) * tuning.contrast_margin;
    return surfaces;
}

ColorRgba color_at(double lightness, double chroma, double hue) {
    OklchColor request;
    request.lightness = std::clamp(lightness, 0.0, 1.0);
    request.chroma = std::min(chroma, max_chroma_in_gamut(request.lightness, hue));
    request.hue = hue;
    return quantize_to_srgb8(request);
}

double chroma_of_step(double lightness, double accent_chroma) {
    const InstrumentTuning& tuning = instrument_tuning();
    const double distance = std::abs(lightness - tuning.accent_chroma_peak);
    const double factor =
        std::clamp(1.0 - tuning.accent_chroma_taper * distance, tuning.accent_chroma_floor, 1.0);
    return accent_chroma * factor;
}

bool clears_surfaces(const ColorRgba& color, const SolveSurfaces& surfaces, double floor) {
    return std::all_of(surfaces.colors.begin(), surfaces.colors.end(),
                       [&](const ColorRgba& s) { return contrast_ratio(color, s) >= floor; });
}

} // namespace tessera::ui::style::detail
