// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace tessera::rendering::image {

// Largest JPEG scale denominator (1, 2, 4 or 8) at which an image of `width` x `height` still has
// at least `target_width` x `target_height` pixels. libjpeg-turbo scales inside the IDCT, so a
// denominator above 1 never allocates the full raster (issue #14).
[[nodiscard]] unsigned jpeg_scale_denominator(std::uint32_t width, std::uint32_t height,
                                              std::uint32_t target_width,
                                              std::uint32_t target_height) noexcept;

struct JpegRaster {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // straight alpha (opaque), top-down
};

// Decodes a baseline or progressive 8-bit JPEG at 1/`denominator` of its size into RGBA8. The
// result is `ceil(width / denominator)` x `ceil(height / denominator)`. Returns false for anything
// this path does not handle (CMYK, non-8-bit data, corrupt data); the caller falls back to the
// general decoder, which is where errors are classified.
[[nodiscard]] bool decode_jpeg_scaled(std::span<const std::uint8_t> bytes, unsigned denominator,
                                      JpegRaster& out);

} // namespace tessera::rendering::image
