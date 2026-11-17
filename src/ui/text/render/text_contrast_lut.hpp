// SPDX-License-Identifier: MIT
//
// The text coverage-contrast LUT (docs/design/display-aware-rendering.md section 3.5; adopted as a
// D2 amendment, docs/design/rendering-production-plan.md). D2 blends text coverage in the 8-bit
// sRGB-encoded space, so light-on-dark text reads thinner and dark-on-light text heavier than
// linear coverage would. The renderer compensates only the thin case: a run's sampled coverage is
// boosted by an amount chosen from the run color's encoded lightness. It is a table, not a formula,
// so the curve has exactly 256 possible values (one per encoded lightness), the frame path costs
// one index and one load, and a golden can pin it.
//
// Internal (not public API): `TextRenderer` is the only caller; a unit test pins the table's shape.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tessera::ui::text::detail {

// Encoded lightness is 0 (black) .. 255 (white); the curve is flat through the mid point and then
// rises to `kContrastMaxBoost` at white. Mid is index 128, i.e. encoded lightness 0.5.
inline constexpr std::size_t kContrastMidIndex = 128;
inline constexpr std::size_t kCoverageContrastLutSize = 256;
inline constexpr float kContrastMaxBoost = 1.4f;

// The curve the table is built from: no boost at or below mid lightness (dark text on a light
// background is already heavy in encoded space), then a linear ramp to 1.4 at white.
[[nodiscard]] constexpr float contrast_boost_for_index(std::size_t index) noexcept {
    if (index <= kContrastMidIndex) {
        return 1.0f;
    }
    const float span = static_cast<float>(kCoverageContrastLutSize - 1 - kContrastMidIndex);
    const float t = (static_cast<float>(index) - static_cast<float>(kContrastMidIndex)) / span;
    return 1.0f + (kContrastMaxBoost - 1.0f) * t;
}

// The table the renderer samples: `kCoverageContrastLut[encoded_lightness]`.
inline constexpr std::array<float, kCoverageContrastLutSize> kCoverageContrastLut = [] {
    std::array<float, kCoverageContrastLutSize> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        table[index] = contrast_boost_for_index(index);
    }
    return table;
}();

// The run color's encoded lightness, quantized to the table's index. The Rec. 601 weights are the
// same proxy the design uses; NaN and out-of-range channels land on black or white, never on an
// out-of-bounds index.
[[nodiscard]] inline std::uint8_t contrast_lut_index(float r, float g, float b) noexcept {
    const float lightness = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (!(lightness > 0.0f)) {
        return 0;
    }
    if (lightness >= 1.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::lround(lightness * 255.0f));
}

} // namespace tessera::ui::text::detail
