// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/image_diff.hpp"

#include <algorithm>
#include <cstdlib>

namespace tessera::rendering::golden {

double ImageDiff::mismatch_ratio() const {
    if (size_mismatch) {
        return 1.0;
    }
    return pixels == 0 ? 0.0 : static_cast<double>(mismatched) / static_cast<double>(pixels);
}

double ImageDiff::mean_channel_delta() const {
    if (size_mismatch || pixels == 0) {
        return 0.0;
    }
    return static_cast<double>(total_channel_delta) / (static_cast<double>(pixels) * 4.0);
}

ImageDiff compare_rgba(ImageSize actual, std::span<const std::uint8_t> actual_pixels,
                       ImageSize expected, std::span<const std::uint8_t> expected_pixels,
                       std::uint32_t per_channel_tolerance) {
    ImageDiff diff;
    diff.actual = actual;
    diff.expected = expected;
    if (actual != expected ||
        actual_pixels.size() < static_cast<std::size_t>(actual.width) * actual.height * 4 ||
        expected_pixels.size() < static_cast<std::size_t>(expected.width) * expected.height * 4) {
        diff.size_mismatch = true;
        return diff;
    }

    diff.pixels = static_cast<std::size_t>(actual.width) * actual.height;
    for (std::size_t pixel = 0; pixel < diff.pixels; ++pixel) {
        bool mismatched = false;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const std::size_t index = pixel * 4 + channel;
            const auto a = static_cast<std::uint32_t>(actual_pixels[index]);
            const auto b = static_cast<std::uint32_t>(expected_pixels[index]);
            const std::uint32_t delta = a > b ? a - b : b - a;
            diff.max_channel_delta = std::max(diff.max_channel_delta, delta);
            diff.total_channel_delta += delta;
            if (delta > per_channel_tolerance) {
                mismatched = true;
            }
        }
        if (mismatched) {
            if (diff.first_mismatch == static_cast<std::size_t>(-1)) {
                diff.first_mismatch = pixel;
            }
            ++diff.mismatched;
        }
    }
    return diff;
}

bool within_tolerance(const ImageDiff& diff, double max_mismatch_ratio) {
    if (diff.size_mismatch) {
        return false;
    }
    const double budget = std::clamp(max_mismatch_ratio, 0.0, 1.0);
    return diff.mismatch_ratio() <= budget;
}

} // namespace tessera::rendering::golden
