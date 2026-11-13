// SPDX-License-Identifier: MIT
//
// Hand-computed unit tests for the perceptual metric (issue #245): SSIM on Rec.709 luma and CIE76
// deltaE in CIELAB. Identical images must give SSIM 1 and deltaE 0; the constants below are derived
// from the documented formulas (docs/specs/golden-tests-v1.md section 4.3) and pinned as literals
// so a formula change cannot pass silently.

#include "tessera/rendering/golden/perceptual.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using tessera::rendering::golden::compute_delta_e;
using tessera::rendering::golden::compute_ssim;
using tessera::rendering::golden::DeltaEStats;
using tessera::rendering::golden::ImageSize;
using tessera::rendering::golden::luma8;
using tessera::rendering::golden::SsimResult;
using tessera::rendering::golden::valid_rgba;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "perceptual_metric: " << message << "\n";
    }
    return condition;
}

bool close(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

std::vector<std::uint8_t> gray_image(ImageSize size, std::uint8_t value) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(size.width) * size.height * 4, value);
}

// Constant gray pixels have luma equal to the channel value (the quantized weights sum to 256).
std::vector<std::uint8_t> luma_image(std::uint32_t width, std::uint32_t height,
                                     std::span<const std::uint8_t> luma) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 255);
    for (std::size_t index = 0; index < luma.size(); ++index) {
        pixels[index * 4 + 0] = luma[index];
        pixels[index * 4 + 1] = luma[index];
        pixels[index * 4 + 2] = luma[index];
    }
    return pixels;
}

bool test_luma() {
    bool ok = true;
    for (const std::uint8_t value : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{64},
                                     std::uint8_t{128}, std::uint8_t{255}}) {
        ok = expect(luma8(value, value, value) == value, "a gray pixel keeps its luma") && ok;
    }
    // Red-only luma: round(0.2126 * 255) with the quantized coefficient 54/256 is 54.
    ok = expect(luma8(255, 0, 0) == 54, "red luma is 54") && ok;
    ok = expect(luma8(0, 0, 0) == 0, "black luma is 0") && ok;
    ok = expect(luma8(255, 255, 255) == 255, "white luma is 255") && ok;
    return ok;
}

bool test_validity() {
    const std::vector<std::uint8_t> pixels(16, 0);
    bool ok = expect(valid_rgba({2, 2}, pixels), "a full buffer is valid");
    ok = expect(!valid_rgba({2, 3}, pixels), "a short buffer is not valid") && ok;
    ok = expect(!valid_rgba({0, 0}, std::span<const std::uint8_t>()), "an empty image is not "
                                                                      "valid") &&
         ok;
    return ok;
}

bool test_ssim_identical() {
    std::array<std::uint8_t, 4 * 16> pixels{};
    for (std::size_t index = 0; index < 16; ++index) {
        pixels[index * 4 + 0] = static_cast<std::uint8_t>(index * 13U + 7U);
        pixels[index * 4 + 1] = static_cast<std::uint8_t>(index * 5U + 3U);
        pixels[index * 4 + 2] = static_cast<std::uint8_t>(index * 3U + 11U);
        pixels[index * 4 + 3] = 255;
    }
    const auto result = compute_ssim({4, 4}, pixels, {4, 4}, pixels);
    bool ok = expect(result.mean == 1.0, "identical images have mean SSIM exactly 1");
    ok = expect(result.worst == 1.0, "identical images have worst-window SSIM exactly 1") && ok;
    ok = expect(result.windows == 1, "a 4x4 image is one clipped 8x8 window") && ok;
    return ok;
}

bool test_ssim_hand_computed() {
    bool ok = true;
    // Constant images have zero variance, so SSIM reduces to (2*m1*m2 + C1) / (m1^2 + m2^2 + C1)
    // with C1 = (0.01 * 255)^2 = 6.5025.
    const auto black = gray_image({2, 2}, 0);
    const auto white = gray_image({2, 2}, 255);
    SsimResult result = compute_ssim({2, 2}, black, {2, 2}, white);
    // 6.5025 / (65025 + 6.5025) = 9.999000099990003e-05
    ok = expect(close(result.mean, 9.999000099990003e-05, 1e-15),
                "SSIM of black against white constant images") &&
         ok;

    const auto hundred = gray_image({2, 2}, 100);
    const auto sixty = gray_image({2, 2}, 60);
    result = compute_ssim({2, 2}, hundred, {2, 2}, sixty);
    // (2*100*60 + 6.5025) / (10000 + 3600 + 6.5025) = 0.8824091642947921
    ok =
        expect(close(result.mean, 0.8824091642947921, 1e-15), "SSIM of gray 100 against gray 60") &&
        ok;

    // 2x2 luma a = [0 10; 0 10], b = [10 20; 10 20]: means 5 and 15, variances 25 and 25,
    // covariance 25. (2*5*15 + C1)(2*25 + C2) / ((25 + 225 + C1)(25 + 25 + C2)) with
    // C1 = 6.5025, C2 = 58.5225 -> 0.610140252044327.
    const std::array<std::uint8_t, 4> luma_a = {0, 10, 0, 10};
    const std::array<std::uint8_t, 4> luma_b = {10, 20, 10, 20};
    const std::vector<std::uint8_t> image_a = luma_image(2, 2, luma_a);
    const std::vector<std::uint8_t> image_b = luma_image(2, 2, luma_b);
    result = compute_ssim({2, 2}, image_a, {2, 2}, image_b);
    ok = expect(close(result.mean, 0.610140252044327, 1e-12),
                "hand-computed variance/covariance SSIM") &&
         ok;

    // A size mismatch is not a measurement: the result keeps the "identical" defaults.
    result = compute_ssim({2, 2}, image_a, {3, 2}, image_b);
    ok = expect(result.windows == 0 && result.mean == 1.0 && result.worst == 1.0,
                "a size mismatch reports no windows") &&
         ok;
    return ok;
}

bool test_delta_e_hand_computed() {
    bool ok = true;
    const auto black = gray_image({2, 2}, 0);
    const auto white = gray_image({2, 2}, 255);

    DeltaEStats result = compute_delta_e({2, 2}, black, {2, 2}, black);
    ok =
        expect(result.mean == 0.0 && result.max == 0.0, "identical images have deltaE exactly 0") &&
        ok;

    result = compute_delta_e({2, 2}, black, {2, 2}, white);
    // L* of sRGB white under CIE D65 is 100.0000039 with the standard matrix/white point; a* and b*
    // are ~1e-5 because the matrix and the white point are not exactly consistent.
    ok =
        expect(close(result.mean, 100.00000386666815, 1e-6), "deltaE black against white is 100") &&
        ok;
    ok =
        expect(close(result.max, 100.00000386666815, 1e-6), "deltaE max black against white") && ok;

    const auto mid = gray_image({2, 2}, 128);
    result = compute_delta_e({2, 2}, black, {2, 2}, mid);
    // L* of sRGB 128 is 53.585015771669404.
    ok =
        expect(close(result.mean, 53.585015771670484, 1e-6), "deltaE black against gray 128") && ok;

    // Alpha is not part of the colour difference.
    const auto clear_black = std::vector<std::uint8_t>(16, 0);
    const auto opaque_black = gray_image({2, 2}, 0);
    result = compute_delta_e({2, 2}, clear_black, {2, 2}, opaque_black);
    ok = expect(result.mean == 0.0 && result.max == 0.0, "deltaE ignores alpha") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_luma() && ok;
    ok = test_validity() && ok;
    ok = test_ssim_identical() && ok;
    ok = test_ssim_hand_computed() && ok;
    ok = test_delta_e_hand_computed() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "perceptual_metric: ok\n";
    return 0;
}
