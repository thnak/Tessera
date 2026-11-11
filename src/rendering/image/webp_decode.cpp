// SPDX-License-Identifier: MIT

#include "webp_decode.hpp"

#include <cstddef>
#include <cstring>
#include <utility>
#include <webp/decode.h>
#include <webp/demux.h>

namespace tessera::rendering::image {
namespace {

[[nodiscard]] std::expected<DecodedImage, ImageError>
decode_first_frame(std::span<const std::uint8_t> bytes, std::uint32_t width, std::uint32_t height,
                   ImageSize wanted) {
    WebPAnimDecoderOptions options;
    if (WebPAnimDecoderOptionsInit(&options) == 0) {
        return std::unexpected(ImageError::malformed);
    }
    options.color_mode = MODE_RGBA; // straight alpha
    options.use_threads = 0;
    const WebPData data{bytes.data(), bytes.size()};
    WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
    if (decoder == nullptr) {
        return std::unexpected(ImageError::malformed);
    }
    std::uint8_t* canvas = nullptr;
    int timestamp = 0;
    const bool got = WebPAnimDecoderGetNext(decoder, &canvas, &timestamp) != 0 && canvas != nullptr;
    DecodedImage full;
    if (got) {
        full.width = width;
        full.height = height;
        full.rgba.assign(canvas, canvas + static_cast<std::size_t>(width) * height * 4U);
    }
    WebPAnimDecoderDelete(decoder);
    if (!got) {
        return std::unexpected(ImageError::malformed);
    }
    full.source_width = width;
    full.source_height = height;
    full.format = ImageFormat::webp;
    if (wanted.width == width && wanted.height == height) {
        return full;
    }
    DecodedImage reduced = downscale_box(full, wanted.width, wanted.height);
    reduced.source_width = width;
    reduced.source_height = height;
    reduced.format = ImageFormat::webp;
    return reduced;
}

} // namespace

bool is_webp(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 12U && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

std::expected<DecodedImage, ImageError> decode_webp_image(std::span<const std::uint8_t> bytes,
                                                          const ImageLimits& limits,
                                                          DecodeTarget target) {
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK ||
        features.width <= 0 || features.height <= 0) {
        return std::unexpected(ImageError::malformed);
    }
    const auto width = static_cast<std::uint32_t>(features.width);
    const auto height = static_cast<std::uint32_t>(features.height);
    if (width > limits.max_width || height > limits.max_height ||
        static_cast<std::uint64_t>(width) * height > limits.max_pixels) {
        return std::unexpected(ImageError::limit_exceeded);
    }
    const ImageSize wanted = fit_decode_size(width, height, target);
    if (features.has_animation != 0) {
        return decode_first_frame(bytes, width, height, wanted);
    }

    DecodedImage image;
    image.width = wanted.width;
    image.height = wanted.height;
    image.source_width = width;
    image.source_height = height;
    image.format = ImageFormat::webp;
    image.rgba.assign(static_cast<std::size_t>(wanted.width) * wanted.height * 4U, 0U);

    WebPDecoderConfig config;
    if (WebPInitDecoderConfig(&config) == 0) {
        return std::unexpected(ImageError::malformed);
    }
    config.output.colorspace = MODE_RGBA; // straight alpha
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = image.rgba.data();
    config.output.u.RGBA.stride = static_cast<int>(wanted.width * 4U);
    config.output.u.RGBA.size = image.rgba.size();
    config.output.width = static_cast<int>(wanted.width);
    config.output.height = static_cast<int>(wanted.height);
    if (wanted.width != width || wanted.height != height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = static_cast<int>(wanted.width);
        config.options.scaled_height = static_cast<int>(wanted.height);
    }
    const VP8StatusCode status = WebPDecode(bytes.data(), bytes.size(), &config);
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK) {
        return std::unexpected(ImageError::malformed);
    }
    return image;
}

} // namespace tessera::rendering::image
