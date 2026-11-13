// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/difference_class.hpp"
#include "tessera/rendering/golden/perceptual.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::rendering::golden {
namespace {

bool any_channel_differs(const std::uint8_t* actual, const std::uint8_t* expected,
                         std::uint32_t& max_delta) noexcept {
    max_delta = 0;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const auto a = static_cast<std::uint32_t>(actual[channel]);
        const auto b = static_cast<std::uint32_t>(expected[channel]);
        max_delta = std::max(max_delta, a > b ? a - b : b - a);
    }
    return max_delta > 0;
}

std::vector<std::uint8_t> luma_plane(ImageSize size, std::span<const std::uint8_t> pixels) {
    std::vector<std::uint8_t> luma(static_cast<std::size_t>(size.width) * size.height);
    for (std::size_t index = 0; index < luma.size(); ++index) {
        luma[index] = luma8(pixels[index * 4], pixels[index * 4 + 1], pixels[index * 4 + 2]);
    }
    return luma;
}

// Marks every pixel whose luma steps by at least `threshold` from a 4-neighbour.
std::vector<bool> gradient_mask(ImageSize size, const std::vector<std::uint8_t>& luma,
                                std::uint32_t threshold) {
    std::vector<bool> gradient(luma.size(), false);
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * size.width + x;
            const std::uint32_t value = luma[index];
            bool stepped = false;
            if (x > 0) {
                const std::uint32_t left = luma[index - 1];
                stepped = stepped || (value > left ? value - left : left - value) >= threshold;
            }
            if (x + 1 < size.width) {
                const std::uint32_t right = luma[index + 1];
                stepped = stepped || (value > right ? value - right : right - value) >= threshold;
            }
            if (y > 0) {
                const std::uint32_t up = luma[index - size.width];
                stepped = stepped || (value > up ? value - up : up - value) >= threshold;
            }
            if (y + 1 < size.height) {
                const std::uint32_t down = luma[index + size.width];
                stepped = stepped || (value > down ? value - down : down - value) >= threshold;
            }
            gradient[index] = stepped;
        }
    }
    return gradient;
}

bool near_gradient(ImageSize size, const std::vector<bool>& gradient, std::uint32_t x,
                   std::uint32_t y, std::uint32_t radius) {
    const std::uint32_t y_begin = y > radius ? y - radius : 0;
    const std::uint32_t y_end = std::min(y + radius + 1, size.height);
    for (std::uint32_t row = y_begin; row < y_end; ++row) {
        const std::uint32_t x_begin = x > radius ? x - radius : 0;
        const std::uint32_t x_end = std::min(x + radius + 1, size.width);
        const std::size_t base = static_cast<std::size_t>(row) * size.width;
        for (std::uint32_t column = x_begin; column < x_end; ++column) {
            if (gradient[base + column]) {
                return true;
            }
        }
    }
    return false;
}

struct BlockCounts {
    std::size_t structural = 0;
    std::size_t edge_rounding = 0;
};

DifferenceRegion region_for_block(std::uint32_t block, std::uint32_t block_x, std::uint32_t block_y,
                                  ImageSize size, std::size_t pixels) {
    DifferenceRegion region;
    region.x = block_x * block;
    region.y = block_y * block;
    region.width = std::min(block, size.width - region.x);
    region.height = std::min(block, size.height - region.y);
    region.pixels = pixels;
    return region;
}

} // namespace

std::string_view to_string(DifferenceClass value) noexcept {
    switch (value) {
    case DifferenceClass::None:
        return "none";
    case DifferenceClass::EdgeRounding:
        return "edge-rounding";
    case DifferenceClass::Structural:
        return "structural";
    }
    return "none";
}

double DifferenceCounts::mismatch_ratio() const noexcept {
    return pixels == 0 ? 0.0 : static_cast<double>(mismatched) / static_cast<double>(pixels);
}

double DifferenceCounts::edge_ratio() const noexcept {
    return pixels == 0 ? 0.0 : static_cast<double>(edge_rounding) / static_cast<double>(pixels);
}

double DifferenceCounts::structural_ratio() const noexcept {
    return pixels == 0 ? 0.0 : static_cast<double>(structural) / static_cast<double>(pixels);
}

DifferenceReport classify_differences(ImageSize actual_size,
                                      std::span<const std::uint8_t> actual_pixels,
                                      ImageSize expected_size,
                                      std::span<const std::uint8_t> expected_pixels,
                                      const DifferenceClassOptions& options) {
    DifferenceReport report;
    report.size = actual_size;
    if (actual_size != expected_size || !valid_rgba(actual_size, actual_pixels) ||
        !valid_rgba(expected_size, expected_pixels)) {
        report.size_mismatch = true;
        return report;
    }

    const std::uint32_t block = std::max(options.block, 1U);
    const std::uint32_t blocks_x = (actual_size.width + block - 1) / block;
    const std::uint32_t blocks_y = (actual_size.height + block - 1) / block;
    std::vector<BlockCounts> blocks(static_cast<std::size_t>(blocks_x) * blocks_y);

    const std::vector<std::uint8_t> actual_luma = luma_plane(actual_size, actual_pixels);
    const std::vector<std::uint8_t> expected_luma = luma_plane(expected_size, expected_pixels);
    const std::vector<bool> actual_gradient =
        gradient_mask(actual_size, actual_luma, options.gradient_threshold);
    const std::vector<bool> expected_gradient =
        gradient_mask(expected_size, expected_luma, options.gradient_threshold);

    report.counts.pixels = static_cast<std::size_t>(actual_size.width) * actual_size.height;
    for (std::uint32_t y = 0; y < actual_size.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * actual_size.width;
        for (std::uint32_t x = 0; x < actual_size.width; ++x) {
            const std::size_t index = row + x;
            const std::size_t base = index * 4;
            std::uint32_t max_delta = 0;
            if (!any_channel_differs(actual_pixels.data() + base, expected_pixels.data() + base,
                                     max_delta)) {
                continue;
            }
            ++report.counts.mismatched;
            const bool on_edge =
                near_gradient(actual_size, actual_gradient, x, y, options.edge_radius) ||
                near_gradient(expected_size, expected_gradient, x, y, options.edge_radius);
            BlockCounts& counts =
                blocks[static_cast<std::size_t>(y / block) * blocks_x + (x / block)];
            if (max_delta <= options.edge_tolerance && on_edge) {
                ++report.counts.edge_rounding;
                ++counts.edge_rounding;
            } else {
                ++report.counts.structural;
                ++counts.structural;
            }
        }
    }

    std::size_t worst_pixels = 0;
    for (std::uint32_t block_y = 0; block_y < blocks_y; ++block_y) {
        for (std::uint32_t block_x = 0; block_x < blocks_x; ++block_x) {
            const BlockCounts& counts =
                blocks[static_cast<std::size_t>(block_y) * blocks_x + block_x];
            const bool structural_wins = report.counts.structural > 0;
            const std::size_t candidate =
                structural_wins ? counts.structural : counts.edge_rounding;
            if (candidate > worst_pixels) {
                worst_pixels = candidate;
                report.worst_class =
                    structural_wins ? DifferenceClass::Structural : DifferenceClass::EdgeRounding;
                report.worst_region =
                    region_for_block(block, block_x, block_y, actual_size, candidate);
            }
        }
    }
    if (worst_pixels == 0) {
        report.worst_class = DifferenceClass::None;
        report.worst_region = DifferenceRegion{};
    }
    return report;
}

} // namespace tessera::rendering::golden
