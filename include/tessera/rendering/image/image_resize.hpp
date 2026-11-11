// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::image {

struct DecodedImage;

// The pixel box an image will be drawn into (display size x DPR), already rounded up to a size
// bucket so that small layout changes keep hitting the same decode (issue #14). A zero width or
// height means "original": decode at source size (`Image decodeSize="original"`).
struct DecodeTarget {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool original() const noexcept {
        return width == 0 || height == 0;
    }
    [[nodiscard]] bool operator==(const DecodeTarget&) const noexcept = default;
};

// Rounds a pixel length up to the next bucket of the sequence 64, 96, 128, 192, 256, 384, ...
// (powers of two and their midpoints), so a bucket is at most 50% larger than the request.
[[nodiscard]] std::uint32_t size_bucket(std::uint32_t pixels) noexcept;

// The target for an image displayed at `display_width` x `display_height` logical pixels on a
// screen with `device_pixel_ratio`: each side is `ceil(display * dpr)`, then bucketed. A
// non-positive size or ratio yields the original target.
[[nodiscard]] DecodeTarget decode_target_for(float display_width, float display_height,
                                             float device_pixel_ratio) noexcept;

// The decoded size for a source of `source_width` x `source_height` and a target: the source is
// scaled by the smallest factor that still covers the target box, keeps its aspect ratio, and is
// never enlarged. An original target, or one at least as large as the source, returns the source
// size.
struct ImageSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
[[nodiscard]] ImageSize fit_decode_size(std::uint32_t source_width, std::uint32_t source_height,
                                        DecodeTarget target) noexcept;

// Area-averaging (box filter) downscale of straight-alpha RGBA8, weighting colors by alpha so
// transparent pixels do not bleed into the result. Pure integer arithmetic: identical input gives
// byte-identical output on every platform. Returns an image with `content_hash` and `format`
// copied and `source_width/height` carried over.
[[nodiscard]] DecodedImage downscale_box(const DecodedImage& source, std::uint32_t width,
                                         std::uint32_t height);

} // namespace tessera::rendering::image
