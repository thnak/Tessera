// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_resize.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::rendering::image {

// Supported raster formats for embedded image assets (documented subset: PNG and JPEG).
enum class ImageFormat : std::uint8_t {
    unknown = 0,
    png = 1,
    jpeg = 2,
};

// Decode limits enforced against the raw input and the decoded raster, so a hostile or accidental
// asset fails with a value instead of allocating unbounded memory.
struct ImageLimits {
    std::size_t max_input_bytes = 8U * 1024U * 1024U; // 8 MiB of encoded bytes
    std::uint32_t max_width = 4096;
    std::uint32_t max_height = 4096;
    std::uint32_t max_pixels = 16U * 1024U * 1024U; // 16 MP decoded raster

    [[nodiscard]] bool operator==(const ImageLimits&) const noexcept = default;
};

enum class ImageError : std::uint8_t {
    invalid_argument = 0, // empty input or zero/nonsense limits
    input_too_large,      // encoded bytes exceed limits.max_input_bytes
    unsupported_format,   // not PNG/JPEG (or a 16-bit PNG we deliberately do not decode yet)
    malformed,            // the decoder rejected the bytes
    limit_exceeded,       // decoded width/height/pixel budget exceeded
};

// A decoded image: tightly packed, straight-alpha RGBA8, top-down, no padding. Decoding is a pure
// function of the bytes and limits (stb_image is deterministic; no threading, no wall clock, no
// filesystem access). 16-bit PNGs are rejected as unsupported in this slice (see deferrals).
struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Size of the encoded image. Equals width/height unless the decode was reduced to a target
    // (issue #14); limits and layout intrinsic size are judged on this, not on the reduced raster.
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    ImageFormat format = ImageFormat::unknown;
    // FNV-1a content hash of the encoded bytes; set by the decode cache and used as the texture
    // key.
    std::uint64_t content_hash = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] std::size_t byte_size() const {
        return rgba.size();
    }
};

// Stable, human-readable format name for diagnostics.
[[nodiscard]] std::string_view format_name(ImageFormat format) noexcept;

// Detects the container from its signature (PNG/JPEG only). Returns unknown for anything else
// without decoding. Used by decode_image and available to callers that want a cheap classification.
[[nodiscard]] ImageFormat detect_format(std::span<const std::uint8_t> bytes) noexcept;

// Decodes PNG/JPEG bytes to RGBA8. Never throws across this seam and never touches the filesystem.
// A corrupt/truncated/unsupported input is a value, not a crash.
//
// With a non-original `target` (display size x DPR bucket, image_resize.hpp) the raster is reduced
// by a deterministic box filter to the smallest size that still covers the target and is never
// enlarged, so what a caller retains is bounded by the display, not the source. Limits apply to the
// encoded image's size. stb_image has no scaled decode, so the full raster exists transiently
// during the call; it is freed before returning.
[[nodiscard]] std::expected<DecodedImage, ImageError>
decode_image(std::span<const std::uint8_t> bytes, ImageLimits limits = {},
             DecodeTarget target = {});

} // namespace tessera::rendering::image
