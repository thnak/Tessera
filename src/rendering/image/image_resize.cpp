// SPDX-License-Identifier: MIT

#include "downscale_kernels.hpp"
#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/image/image_resize.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tessera::rendering::image {

std::uint32_t size_bucket(std::uint32_t pixels) noexcept {
    constexpr std::uint32_t kFirst = 64;
    constexpr std::uint32_t kLimit = 1U << 30;
    if (pixels <= kFirst) {
        return kFirst;
    }
    for (std::uint32_t power = kFirst; power < kLimit; power *= 2) {
        if (pixels <= power) {
            return power;
        }
        if (pixels <= power + power / 2) {
            return power + power / 2;
        }
    }
    return pixels;
}

DecodeTarget decode_target_for(float display_width, float display_height,
                               float device_pixel_ratio) noexcept {
    if (!(display_width > 0.0F) || !(display_height > 0.0F) || !(device_pixel_ratio > 0.0F) ||
        display_width * device_pixel_ratio > 65536.0F ||
        display_height * device_pixel_ratio > 65536.0F) {
        return {};
    }
    return {
        size_bucket(static_cast<std::uint32_t>(std::ceil(display_width * device_pixel_ratio))),
        size_bucket(static_cast<std::uint32_t>(std::ceil(display_height * device_pixel_ratio)))};
}

ImageSize fit_decode_size(std::uint32_t source_width, std::uint32_t source_height,
                          DecodeTarget target) noexcept {
    if (target.original() || source_width == 0 || source_height == 0 ||
        (target.width >= source_width && target.height >= source_height)) {
        return {source_width, source_height};
    }
    // Cover the target box: the larger of the two ratios, compared as exact fractions.
    // factor = max(target.width / source_width, target.height / source_height).
    const std::uint64_t by_width = static_cast<std::uint64_t>(target.width) * source_height;
    const std::uint64_t by_height = static_cast<std::uint64_t>(target.height) * source_width;
    std::uint64_t numerator = target.width;
    std::uint64_t denominator = source_width;
    if (by_height > by_width) {
        numerator = target.height;
        denominator = source_height;
    }
    const auto scaled = [&](std::uint32_t side) {
        const std::uint64_t value =
            (static_cast<std::uint64_t>(side) * numerator * 2 + denominator) /
            (denominator * 2); // round to nearest
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(value, 1, side));
    };
    return {scaled(source_width), scaled(source_height)};
}

DecodedImage downscale_box(const DecodedImage& source, std::uint32_t width, std::uint32_t height) {
    return downscale_box_with(best_isa(), source, width, height);
}

} // namespace tessera::rendering::image
