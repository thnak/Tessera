// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"

#include <cstdint>
#include <expected>

namespace tessera::rendering::path {

// Document-level texture identity: one texture per (document identity, logical size, raster scale).
//
// Batching rule: a static SVG document is composited in one raster and uploaded as one texture, so
// a whole icon is one draw call, overlapping shapes and group opacity composite exactly as the
// document specifies, and the decoded geometry is reused across all size/scale variants. Per-shape
// textures (`PathTextureCache::acquire(key, PathShape)`) still win when shapes move or restyle
// independently, when per-shape damage/hit regions matter, or when one shape is shared by several
// documents; they are also the right tool for a single-path icon where the document adds nothing.
struct SvgTextureKey {
    std::uint64_t document_id = 0;
    std::uint32_t document_version = 0; // bump when the decoded document changes
    // Logical box the texture is drawn into, in device pixels (normally document.width/height).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t scale_milli = 1000; // raster scale x1000

    [[nodiscard]] bool operator==(const SvgTextureKey&) const noexcept = default;
};

// Fills width/height from the document; pass a different logical box only if the caller lays the
// document out at another size (the raster is still the document's own coordinates scaled up).
[[nodiscard]] SvgTextureKey make_svg_texture_key(std::uint64_t document_id,
                                                 std::uint32_t document_version,
                                                 const SvgDocument& document,
                                                 std::uint32_t scale_milli = 1000) noexcept;

// Rasterizes and uploads the whole document through `PathTextureCache` (reusing its bounded LRU,
// UNORM/sRGB format rule and D7 device-generation handling). The texture is marked as a document
// composite so it cannot collide with a single-shape key of the same id/version/size/scale.
[[nodiscard]] std::expected<CachedPathTexture, PathTextureError>
acquire_svg_texture(PathTextureCache& cache, const SvgTextureKey& key, const SvgDocument& document);

} // namespace tessera::rendering::path
