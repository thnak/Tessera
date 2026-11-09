// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/svg_texture.hpp"

#include <cstdint>
#include <expected>
#include <span>

namespace tessera::rendering::path {

SvgTextureKey make_svg_texture_key(std::uint64_t document_id, std::uint32_t document_version,
                                   const SvgDocument& document,
                                   std::uint32_t scale_milli) noexcept {
    SvgTextureKey key;
    key.document_id = document_id;
    key.document_version = document_version;
    key.width = document.width;
    key.height = document.height;
    key.scale_milli = scale_milli;
    return key;
}

std::expected<CachedPathTexture, PathTextureError>
acquire_svg_texture(PathTextureCache& cache, const SvgTextureKey& key,
                    const SvgDocument& document) {
    if (key.width == 0U || key.height == 0U || key.scale_milli == 0U) {
        return std::unexpected(PathTextureError::invalid_request);
    }

    // The document is the content identity, so the per-shape fill/stroke signature is unused; the
    // `document` flag keeps this key disjoint from single-shape keys in the same cache.
    PathTextureKey texture_key;
    texture_key.path_id = key.document_id;
    texture_key.path_version = key.document_version;
    texture_key.width = key.width;
    texture_key.height = key.height;
    texture_key.scale_milli = key.scale_milli;
    texture_key.fill_rule = FillRule::non_zero;
    texture_key.document = true;
    return cache.acquire(texture_key, std::span<const PathShape>(document.shapes));
}

} // namespace tessera::rendering::path
