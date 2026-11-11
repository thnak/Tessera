// SPDX-License-Identifier: MIT

#include "tessera/ui/assets/asset_image.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::ui::assets {
namespace {

[[nodiscard]] AssetImageError map_error(rendering::image::ImageError error) noexcept {
    switch (error) {
    case rendering::image::ImageError::invalid_argument:
        return AssetImageError::malformed;
    case rendering::image::ImageError::input_too_large:
        return AssetImageError::input_too_large;
    case rendering::image::ImageError::unsupported_format:
        return AssetImageError::unsupported_format;
    case rendering::image::ImageError::malformed:
        return AssetImageError::malformed;
    case rendering::image::ImageError::limit_exceeded:
        return AssetImageError::limit_exceeded;
    }
    return AssetImageError::malformed;
}

[[nodiscard]] std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

} // namespace

std::expected<std::shared_ptr<const rendering::image::DecodedImage>, AssetImageError>
load_asset_image(const AssetBundle& bundle, std::string_view name,
                 rendering::image::ImageDecodeCache& cache, rendering::image::ImageLimits limits,
                 rendering::image::DecodeTarget target) {
    const auto asset = bundle.find(name);
    if (!asset.has_value()) {
        return std::unexpected(asset.error() == AssetError::invalid_name
                                   ? AssetImageError::invalid_name
                                   : AssetImageError::not_found);
    }
    auto decoded = cache.acquire(as_u8(asset->bytes), limits, target);
    if (!decoded.has_value()) {
        return std::unexpected(map_error(decoded.error()));
    }
    return *std::move(decoded);
}

} // namespace tessera::ui::assets
