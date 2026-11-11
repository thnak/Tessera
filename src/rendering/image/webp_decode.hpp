// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_loader.hpp"

#include <cstdint>
#include <expected>
#include <span>

namespace tessera::rendering::image {

// True when `bytes` start with a RIFF container whose form type is WEBP.
[[nodiscard]] bool is_webp(std::span<const std::uint8_t> bytes) noexcept;

// Decodes a WebP (lossy, lossless or with alpha) to straight-alpha RGBA8 through libwebp. Limits
// are judged on the encoded size before any pixel is allocated. A still image is reduced inside the
// decoder (libwebp's scaler) straight into the result, so the full raster never exists; an animated
// image decodes its first frame composed on the canvas (the documented behavior), then goes through
// the box filter. A non-original target never enlarges.
[[nodiscard]] std::expected<DecodedImage, ImageError>
decode_webp_image(std::span<const std::uint8_t> bytes, const ImageLimits& limits,
                  DecodeTarget target);

} // namespace tessera::rendering::image
