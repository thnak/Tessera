// SPDX-License-Identifier: MIT

#include "tessera/ui/assets/asset_svg.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace tessera::ui::assets {
namespace {

namespace path = tessera::rendering::path;

// The SVG loader takes a byte span of std::uint8_t; std::byte is the bundle's storage type. The
// standard allows char-like types (including std::byte and unsigned char) to alias any object, so
// this is a view cast, not a copy.
[[nodiscard]] std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

[[nodiscard]] AssetSvgError map_svg_error(path::SvgError error) noexcept {
    switch (error) {
    case path::SvgError::invalid_argument:
        return AssetSvgError::invalid_name;
    case path::SvgError::input_too_large:
        return AssetSvgError::input_too_large;
    case path::SvgError::malformed:
        return AssetSvgError::malformed;
    case path::SvgError::unsupported:
        return AssetSvgError::unsupported;
    case path::SvgError::limit_exceeded:
        return AssetSvgError::limit_exceeded;
    case path::SvgError::backend_unavailable:
        return AssetSvgError::backend_unavailable;
    case path::SvgError::backend_failure:
        break;
    }
    return AssetSvgError::backend_failure;
}

[[nodiscard]] AssetSvgError map_asset_error(AssetError error) noexcept {
    return error == AssetError::not_found ? AssetSvgError::not_found : AssetSvgError::invalid_name;
}

[[nodiscard]] AssetSvgError map_texture_error(path::PathTextureError error) noexcept {
    switch (error) {
    case path::PathTextureError::invalid_request:
        return AssetSvgError::invalid_name;
    case path::PathTextureError::entry_too_large:
        return AssetSvgError::limit_exceeded;
    case path::PathTextureError::device_lost:
        return AssetSvgError::device_lost;
    case path::PathTextureError::rasterize_failed:
    case path::PathTextureError::upload_failed:
        break;
    }
    return AssetSvgError::texture_failed;
}

} // namespace

std::expected<const path::SvgDocument*, AssetSvgError> load_asset_svg(const AssetBundle& bundle,
                                                                      std::string_view name,
                                                                      path::SvgDocumentCache& cache,
                                                                      path::SvgLimits limits) {
    const auto asset = bundle.find(name);
    if (!asset.has_value()) {
        return std::unexpected(map_asset_error(asset.error()));
    }
    // The content hash keys the document cache, so two names for the same bytes share one decode.
    const path::SvgDocumentKey key = path::make_svg_document_key(as_u8(asset->bytes), limits);
    auto document = cache.acquire(key, as_u8(asset->bytes));
    if (!document.has_value()) {
        return std::unexpected(map_svg_error(document.error()));
    }
    return document.value();
}

std::expected<AssetSvgTexture, AssetSvgError>
load_asset_svg_texture(const AssetBundle& bundle, std::string_view name,
                       path::SvgDocumentCache& cache, path::PathTextureCache& textures,
                       std::uint32_t scale_milli, path::SvgLimits limits) {
    const auto asset = bundle.find(name);
    if (!asset.has_value()) {
        return std::unexpected(map_asset_error(asset.error()));
    }
    const path::SvgDocumentKey document_key =
        path::make_svg_document_key(as_u8(asset->bytes), limits);
    auto document = cache.acquire(document_key, as_u8(asset->bytes));
    if (!document.has_value()) {
        return std::unexpected(map_svg_error(document.error()));
    }

    const path::SvgTextureKey texture_key =
        path::make_svg_texture_key(document_key.content_hash, 0, *document.value(), scale_milli);
    auto texture = path::acquire_svg_texture(textures, texture_key, *document.value());
    if (!texture.has_value()) {
        return std::unexpected(map_texture_error(texture.error()));
    }
    return AssetSvgTexture{document.value(), *texture};
}

} // namespace tessera::ui::assets
