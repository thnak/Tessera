// SPDX-License-Identifier: MIT

#include "tessera/harness/image_compare.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <stb_image.h>
#include <stb_image_write.h>

namespace tessera::harness {
namespace {

void append_chunk(void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                               static_cast<std::size_t>(size));
}

ImageError error(ImageErrorCode code, std::string message) {
    return ImageError{code, std::move(message)};
}

} // namespace

std::expected<RgbaImage, ImageError> decode_png(std::span<const std::uint8_t> bytes) {
    int width = 0;
    int height = 0;
    int channels = 0;
    if (bytes.empty() ||
        stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                              &channels) == 0 ||
        width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > kMaxImageDimension ||
        static_cast<std::uint32_t>(height) > kMaxImageDimension) {
        return std::unexpected(error(ImageErrorCode::InvalidPng, "not a decodable PNG"));
    }
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width,
                                            &height, &channels, 4);
    if (pixels == nullptr) {
        return std::unexpected(error(ImageErrorCode::InvalidPng, "PNG decode failed"));
    }
    RgbaImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}

std::expected<std::string, ImageError> encode_png(const RgbaImage& image) {
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return std::unexpected(error(ImageErrorCode::BadImage, "image is empty or truncated"));
    }
    std::string out;
    const int ok = stbi_write_png_to_func(&append_chunk, &out, static_cast<int>(image.width),
                                          static_cast<int>(image.height), 4, image.rgba.data(),
                                          static_cast<int>(image.width) * 4);
    if (ok == 0 || out.empty()) {
        return std::unexpected(error(ImageErrorCode::EncodeFailed, "PNG encode failed"));
    }
    return out;
}

} // namespace tessera::harness
