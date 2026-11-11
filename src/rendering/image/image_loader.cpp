// SPDX-License-Identifier: MIT

#include "jpeg_scaled.hpp"
#include "tessera/rendering/image/image_loader.hpp"
#include "webp_decode.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include <stb_image.h>

namespace tessera::rendering::image {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 0x50, 0x4E, 0x47,
                                                       0x0D, 0x0A, 0x1A, 0x0A};

[[nodiscard]] bool is_jpeg(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U && bytes[2] == 0xFFU;
}

} // namespace

std::string_view format_name(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::png:
        return "png";
    case ImageFormat::jpeg:
        return "jpeg";
    case ImageFormat::webp:
        return "webp";
    case ImageFormat::unknown:
        break;
    }
    return "unknown";
}

ImageFormat detect_format(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() >= kPngSignature.size() &&
        std::memcmp(bytes.data(), kPngSignature.data(), kPngSignature.size()) == 0) {
        return ImageFormat::png;
    }
    if (is_jpeg(bytes)) {
        return ImageFormat::jpeg;
    }
    if (is_webp(bytes)) {
        return ImageFormat::webp;
    }
    return ImageFormat::unknown;
}

std::expected<DecodedImage, ImageError> decode_image(std::span<const std::uint8_t> bytes,
                                                     ImageLimits limits, DecodeTarget target) {
    if (bytes.empty() || limits.max_input_bytes == 0U || limits.max_width == 0U ||
        limits.max_height == 0U || limits.max_pixels == 0U) {
        return std::unexpected(ImageError::invalid_argument);
    }
    if (bytes.size() > limits.max_input_bytes) {
        return std::unexpected(ImageError::input_too_large);
    }
    const ImageFormat format = detect_format(bytes);
    if (format == ImageFormat::unknown) {
        return std::unexpected(ImageError::unsupported_format);
    }

    if (format == ImageFormat::webp) {
        return decode_webp_image(bytes, limits, target);
    }

    int encoded_width = 0;
    int encoded_height = 0;
    int encoded_channels = 0;
    // stb reads the header only; this bounds the allocation before the full decode.
    if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &encoded_width,
                              &encoded_height, &encoded_channels) == 0 ||
        encoded_width <= 0 || encoded_height <= 0) {
        return std::unexpected(ImageError::malformed);
    }
    if (static_cast<std::uint32_t>(encoded_width) > limits.max_width ||
        static_cast<std::uint32_t>(encoded_height) > limits.max_height) {
        return std::unexpected(ImageError::limit_exceeded);
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(encoded_width) * static_cast<std::uint64_t>(encoded_height);
    if (pixel_count > limits.max_pixels) {
        return std::unexpected(ImageError::limit_exceeded);
    }
    // This slice decodes 8-bit inputs only; 16-bit PNGs are rejected rather than silently narrowed.
    if (stbi_is_16_bit_from_memory(bytes.data(), static_cast<int>(bytes.size())) == 1) {
        return std::unexpected(ImageError::unsupported_format);
    }

    // A JPEG shown smaller than its source is decoded at 1/2, 1/4 or 1/8 inside the IDCT, so the
    // full raster is never allocated; anything that path declines falls through to stb below.
    if (format == ImageFormat::jpeg && !target.original()) {
        const std::uint32_t source_width = static_cast<std::uint32_t>(encoded_width);
        const std::uint32_t source_height = static_cast<std::uint32_t>(encoded_height);
        const ImageSize wanted = fit_decode_size(source_width, source_height, target);
        const unsigned denominator =
            jpeg_scale_denominator(source_width, source_height, wanted.width, wanted.height);
        JpegRaster scaled;
        if (denominator > 1U && decode_jpeg_scaled(bytes, denominator, scaled)) {
            DecodedImage image;
            image.width = scaled.width;
            image.height = scaled.height;
            image.source_width = source_width;
            image.source_height = source_height;
            image.format = format;
            image.rgba = std::move(scaled.rgba);
            if (image.width != wanted.width || image.height != wanted.height) {
                DecodedImage reduced = downscale_box(image, wanted.width, wanted.height);
                reduced.source_width = source_width;
                reduced.source_height = source_height;
                reduced.format = format;
                return reduced;
            }
            return image;
        }
    }

    int decoded_width = 0;
    int decoded_height = 0;
    int decoded_channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                             &decoded_width, &decoded_height, &decoded_channels, 4);
    if (decoded == nullptr || decoded_width <= 0 || decoded_height <= 0) {
        return std::unexpected(ImageError::malformed);
    }
    const std::uint64_t decoded_pixels =
        static_cast<std::uint64_t>(decoded_width) * static_cast<std::uint64_t>(decoded_height);
    if (static_cast<std::uint32_t>(decoded_width) > limits.max_width ||
        static_cast<std::uint32_t>(decoded_height) > limits.max_height ||
        decoded_pixels > limits.max_pixels) {
        stbi_image_free(decoded);
        return std::unexpected(ImageError::limit_exceeded);
    }

    DecodedImage image;
    image.width = static_cast<std::uint32_t>(decoded_width);
    image.height = static_cast<std::uint32_t>(decoded_height);
    image.source_width = image.width;
    image.source_height = image.height;
    image.format = format;
    const ImageSize reduced = fit_decode_size(image.width, image.height, target);
    if (reduced.width == image.width && reduced.height == image.height) {
        image.rgba.resize(static_cast<std::size_t>(decoded_pixels) * 4U);
        std::memcpy(image.rgba.data(), decoded, image.rgba.size());
        stbi_image_free(decoded);
        return image;
    }
    // Reduce straight from stb's buffer so only the small raster is ever copied into a vector.
    DecodedImage full;
    full.width = image.width;
    full.height = image.height;
    full.rgba.assign(decoded, decoded + static_cast<std::size_t>(decoded_pixels) * 4U);
    stbi_image_free(decoded);
    image = downscale_box(full, reduced.width, reduced.height);
    image.format = format;
    return image;
}

} // namespace tessera::rendering::image
