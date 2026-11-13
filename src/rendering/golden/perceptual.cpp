// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/perceptual.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::rendering::golden {
namespace {

struct Lab {
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

// IEC 61966-2-1 sRGB transfer function, inverse (encoded 8-bit -> linear light in [0, 1]).
double srgb_to_linear(std::uint8_t channel) noexcept {
    const double encoded = static_cast<double>(channel) / 255.0;
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

// CIELAB companding for a tristimulus ratio (CIE 15:2004), D65 white.
double lab_compand(double ratio) noexcept {
    constexpr double kEpsilon = 216.0 / 24389.0; // (6/29)^3
    constexpr double kKappa = 24389.0 / 27.0;    // (29/3)^3
    if (ratio > kEpsilon) {
        return std::cbrt(ratio);
    }
    return (kKappa * ratio + 16.0) / 116.0;
}

// sRGB -> linear -> CIE XYZ (D65) -> CIELAB. The coefficients and white point are the CIE/SMPTE
// values documented in the spec; they are constants here so the same numbers are used on every
// platform.
Lab to_lab(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    const double linear_r = srgb_to_linear(r);
    const double linear_g = srgb_to_linear(g);
    const double linear_b = srgb_to_linear(b);

    const double x = 0.4124564 * linear_r + 0.3575761 * linear_g + 0.1804375 * linear_b;
    const double y = 0.2126729 * linear_r + 0.7151522 * linear_g + 0.0721750 * linear_b;
    const double z = 0.0193339 * linear_r + 0.1191920 * linear_g + 0.9503041 * linear_b;

    constexpr double kWhiteX = 0.95047;
    constexpr double kWhiteY = 1.0;
    constexpr double kWhiteZ = 1.08883;

    const double fx = lab_compand(x / kWhiteX);
    const double fy = lab_compand(y / kWhiteY);
    const double fz = lab_compand(z / kWhiteZ);

    Lab lab;
    lab.l = 116.0 * fy - 16.0;
    lab.a = 500.0 * (fx - fy);
    lab.b = 200.0 * (fy - fz);
    return lab;
}

std::vector<double> luma_plane(ImageSize size, std::span<const std::uint8_t> pixels) {
    std::vector<double> luma(static_cast<std::size_t>(size.width) * size.height);
    for (std::size_t index = 0; index < luma.size(); ++index) {
        luma[index] = static_cast<double>(
            luma8(pixels[index * 4], pixels[index * 4 + 1], pixels[index * 4 + 2]));
    }
    return luma;
}

} // namespace

std::uint8_t luma8(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    // 54/256, 183/256 and 19/256 are Rec.709 luma quantized; their sum is exactly 256, so a gray
    // pixel (r == g == b) maps back to itself.
    const std::uint32_t weighted = 54U * static_cast<std::uint32_t>(r) +
                                   183U * static_cast<std::uint32_t>(g) +
                                   19U * static_cast<std::uint32_t>(b) + 128U;
    return static_cast<std::uint8_t>(weighted >> 8U);
}

bool valid_rgba(ImageSize size, std::span<const std::uint8_t> pixels) noexcept {
    if (size.width == 0 || size.height == 0) {
        return false;
    }
    return pixels.size() >= static_cast<std::size_t>(size.width) * size.height * 4;
}

SsimResult compute_ssim(ImageSize actual_size, std::span<const std::uint8_t> actual_pixels,
                        ImageSize expected_size, std::span<const std::uint8_t> expected_pixels,
                        const SsimOptions& options) {
    SsimResult result;
    if (actual_size != expected_size || !valid_rgba(actual_size, actual_pixels) ||
        !valid_rgba(expected_size, expected_pixels) || options.window == 0 || options.l <= 0.0) {
        return result;
    }

    const std::vector<double> actual = luma_plane(actual_size, actual_pixels);
    const std::vector<double> expected = luma_plane(expected_size, expected_pixels);

    const double c1 = (options.k1 * options.l) * (options.k1 * options.l);
    const double c2 = (options.k2 * options.l) * (options.k2 * options.l);
    const std::uint32_t window = options.window;

    double total = 0.0;
    std::uint32_t windows = 0;
    double worst = 1.0;
    for (std::uint32_t y0 = 0; y0 < actual_size.height; y0 += window) {
        const std::uint32_t y1 = std::min(y0 + window, actual_size.height);
        for (std::uint32_t x0 = 0; x0 < actual_size.width; x0 += window) {
            const std::uint32_t x1 = std::min(x0 + window, actual_size.width);
            const double count = static_cast<double>((y1 - y0)) * static_cast<double>((x1 - x0));
            double sum_actual = 0.0;
            double sum_expected = 0.0;
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * actual_size.width;
                for (std::uint32_t x = x0; x < x1; ++x) {
                    sum_actual += actual[row + x];
                    sum_expected += expected[row + x];
                }
            }
            const double mean_actual = sum_actual / count;
            const double mean_expected = sum_expected / count;
            double variance_actual = 0.0;
            double variance_expected = 0.0;
            double covariance = 0.0;
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * actual_size.width;
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const double delta_actual = actual[row + x] - mean_actual;
                    const double delta_expected = expected[row + x] - mean_expected;
                    variance_actual += delta_actual * delta_actual;
                    variance_expected += delta_expected * delta_expected;
                    covariance += delta_actual * delta_expected;
                }
            }
            variance_actual /= count;
            variance_expected /= count;
            covariance /= count;

            const double numerator =
                (2.0 * mean_actual * mean_expected + c1) * (2.0 * covariance + c2);
            const double denominator =
                (mean_actual * mean_actual + mean_expected * mean_expected + c1) *
                (variance_actual + variance_expected + c2);
            const double ssim = denominator == 0.0 ? 1.0 : numerator / denominator;
            total += ssim;
            ++windows;
            worst = std::min(worst, ssim);
        }
    }

    result.mean = windows == 0 ? 1.0 : total / static_cast<double>(windows);
    result.worst = windows == 0 ? 1.0 : worst;
    result.windows = windows;
    return result;
}

DeltaEStats compute_delta_e(ImageSize actual_size, std::span<const std::uint8_t> actual_pixels,
                            ImageSize expected_size,
                            std::span<const std::uint8_t> expected_pixels) {
    DeltaEStats result;
    if (actual_size != expected_size || !valid_rgba(actual_size, actual_pixels) ||
        !valid_rgba(expected_size, expected_pixels)) {
        return result;
    }

    const std::size_t pixels = static_cast<std::size_t>(actual_size.width) * actual_size.height;
    double total = 0.0;
    for (std::size_t index = 0; index < pixels; ++index) {
        const std::size_t base = index * 4;
        const Lab actual =
            to_lab(actual_pixels[base], actual_pixels[base + 1], actual_pixels[base + 2]);
        const Lab expected =
            to_lab(expected_pixels[base], expected_pixels[base + 1], expected_pixels[base + 2]);
        const double delta_l = actual.l - expected.l;
        const double delta_a = actual.a - expected.a;
        const double delta_b = actual.b - expected.b;
        const double delta_e = std::sqrt(delta_l * delta_l + delta_a * delta_a + delta_b * delta_b);
        total += delta_e;
        result.max = std::max(result.max, delta_e);
    }
    result.mean = static_cast<double>(pixels) == 0.0 ? 0.0 : total / static_cast<double>(pixels);
    return result;
}

} // namespace tessera::rendering::golden
