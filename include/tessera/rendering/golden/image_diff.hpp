// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::rendering::golden {

struct ImageSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool operator==(const ImageSize& other) const {
        return width == other.width && height == other.height;
    }
};

struct ImageDiff {
    ImageSize actual;
    ImageSize expected;
    std::size_t pixels = 0;
    std::size_t mismatched = 0;
    std::size_t first_mismatch = static_cast<std::size_t>(-1);
    std::uint32_t max_channel_delta = 0;
    std::uint64_t total_channel_delta = 0;
    bool size_mismatch = false;

    [[nodiscard]] double mismatch_ratio() const;
    [[nodiscard]] double mean_channel_delta() const;
};

// Compares two RGBA8 images (tightly packed, 4 bytes/pixel). A pixel counts as mismatched when any
// channel differs by more than `per_channel_tolerance`.
[[nodiscard]] ImageDiff compare_rgba(ImageSize actual, std::span<const std::uint8_t> actual_pixels,
                                     ImageSize expected,
                                     std::span<const std::uint8_t> expected_pixels,
                                     std::uint32_t per_channel_tolerance = 0);

// True when the images have the same dimensions and the mismatch ratio is within the budget.
[[nodiscard]] bool within_tolerance(const ImageDiff& diff, double max_mismatch_ratio);

} // namespace tessera::rendering::golden
