// SPDX-License-Identifier: MIT

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
    DecodedImage out;
    out.format = source.format;
    out.content_hash = source.content_hash;
    out.source_width = source.source_width != 0 ? source.source_width : source.width;
    out.source_height = source.source_height != 0 ? source.source_height : source.height;
    if (width == 0 || height == 0 || source.width == 0 || source.height == 0) {
        return out;
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4U);

    for (std::uint32_t oy = 0; oy < height; ++oy) {
        const std::uint32_t y0 =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(oy) * source.height / height);
        const std::uint32_t y1 =
            std::max(y0 + 1, static_cast<std::uint32_t>(
                                 (static_cast<std::uint64_t>(oy + 1) * source.height + height - 1) /
                                 height));
        for (std::uint32_t ox = 0; ox < width; ++ox) {
            const std::uint32_t x0 =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(ox) * source.width / width);
            const std::uint32_t x1 = std::max(
                x0 + 1,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(ox + 1) * source.width + width - 1) / width));
            std::uint64_t sum_r = 0;
            std::uint64_t sum_g = 0;
            std::uint64_t sum_b = 0;
            std::uint64_t sum_a = 0;
            for (std::uint32_t y = y0; y < y1 && y < source.height; ++y) {
                for (std::uint32_t x = x0; x < x1 && x < source.width; ++x) {
                    const std::uint8_t* p =
                        &source.rgba[(static_cast<std::size_t>(y) * source.width + x) * 4U];
                    sum_r += static_cast<std::uint64_t>(p[0]) * p[3];
                    sum_g += static_cast<std::uint64_t>(p[1]) * p[3];
                    sum_b += static_cast<std::uint64_t>(p[2]) * p[3];
                    sum_a += p[3];
                }
            }
            const std::uint64_t count =
                static_cast<std::uint64_t>(std::min(y1, source.height) - y0) *
                (std::min(x1, source.width) - x0);
            std::uint8_t* q = &out.rgba[(static_cast<std::size_t>(oy) * width + ox) * 4U];
            if (sum_a == 0) {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            q[0] = static_cast<std::uint8_t>((sum_r + sum_a / 2) / sum_a);
            q[1] = static_cast<std::uint8_t>((sum_g + sum_a / 2) / sum_a);
            q[2] = static_cast<std::uint8_t>((sum_b + sum_a / 2) / sum_a);
            q[3] = static_cast<std::uint8_t>((sum_a + count / 2) / count);
        }
    }
    return out;
}

} // namespace tessera::rendering::image
