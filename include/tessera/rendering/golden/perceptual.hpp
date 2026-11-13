// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/golden/image_diff.hpp"

#include <cstdint>
#include <span>

namespace tessera::rendering::golden {

// Perceptual measures that sit next to the per-channel delta (compare_rgba). They are
// cross-backend diagnostics, not a golden update path: docs/specs/golden-tests-v1.md section 4.
// compare_rgba and its callers are unchanged by these functions.

// Rec.709 luma of one 8-bit sRGB-encoded pixel, with the standard coefficients quantized to
// 256ths: (54 R + 183 G + 19 B + 128) >> 8. The rounding makes an achromatic pixel keep its
// channel value exactly (54 + 183 + 19 = 256), which keeps the unit tests hand-computable.
[[nodiscard]] std::uint8_t luma8(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

// SSIM over luma (Wang et al. 2004) with uniform square windows and population moments. Identical
// inputs return exactly 1.0 for every window, so an "exact" pair policy can require 1.0.
struct SsimOptions {
    std::uint32_t window = 8; // square window side in pixels; the edge windows are clipped
    double k1 = 0.01;         // luminance stabilizer, C1 = (k1 * L)^2
    double k2 = 0.03;         // contrast/structure stabilizer, C2 = (k2 * L)^2
    double l = 255.0;         // dynamic range of the luma signal
};

struct SsimResult {
    double mean = 1.0;  // mean window SSIM (the headline number)
    double worst = 1.0; // lowest window SSIM, for "where did it break" reports
    std::uint32_t windows = 0;
};

// Returns mean=1, worst=1, windows=0 for invalid or empty inputs; callers validate sizes first
// (valid_rgba).
[[nodiscard]] SsimResult compute_ssim(ImageSize actual_size,
                                      std::span<const std::uint8_t> actual_pixels,
                                      ImageSize expected_size,
                                      std::span<const std::uint8_t> expected_pixels,
                                      const SsimOptions& options = {});

struct DeltaEStats {
    double mean = 0.0; // mean CIE76 deltaE over every pixel
    double max = 0.0;  // largest per-pixel deltaE
};

// CIE76 deltaE, the Euclidean distance in CIELAB. The documented pipeline (sRGB IEC 61966-2-1 ->
// linear light -> CIE XYZ under D65 -> CIELAB) lives in docs/specs/golden-tests-v1.md section 4.3.
// Alpha is ignored: cross-backend parity compares painted color, not coverage storage.
// Identical inputs return exactly 0.0 for every pixel.
[[nodiscard]] DeltaEStats compute_delta_e(ImageSize actual_size,
                                          std::span<const std::uint8_t> actual_pixels,
                                          ImageSize expected_size,
                                          std::span<const std::uint8_t> expected_pixels);

// True when both spans can be read as tightly packed RGBA8 of `size`.
[[nodiscard]] bool valid_rgba(ImageSize size, std::span<const std::uint8_t> pixels) noexcept;

} // namespace tessera::rendering::golden
