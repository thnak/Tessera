// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_bundle_compose.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace tessera::ui::assets {

// Loads an embedded SVG by name through the decoded-document cache. Failure modes are values:
// - not_found: no such asset (the documented error for a missing asset);
// - invalid_name: the requested name is not normalized (see normalize_asset_name);
// - the remaining values mirror SvgError from the decode (malformed/unsupported/too large/limit/
//   backend unavailable/backend failure). No exception crosses this seam and no partial document
//   is cached.
enum class AssetSvgError : std::uint8_t {
    not_found = 0,
    invalid_name,
    input_too_large,
    malformed,
    unsupported,
    limit_exceeded,
    backend_unavailable,
    backend_failure,
    texture_failed,
    device_lost,
};

[[nodiscard]] std::expected<const rendering::path::SvgDocument*, AssetSvgError>
load_asset_svg(const AssetBundle& bundle, std::string_view name,
               rendering::path::SvgDocumentCache& cache, rendering::path::SvgLimits limits = {});

// Composed lookup for apps whose generated vector bundle and project manifest bundle are separate
// (see ComposedAssetBundle): resolves the name first-bundle-wins, then decodes it exactly like the
// single-bundle overload. The returned document is cached by asset content, so two names for the
// same bytes still share one decode.
[[nodiscard]] std::expected<const rendering::path::SvgDocument*, AssetSvgError>
load_asset_svg(const ComposedAssetBundle& bundles, std::string_view name,
               rendering::path::SvgDocumentCache& cache, rendering::path::SvgLimits limits = {});

struct AssetSvgTexture {
    const rendering::path::SvgDocument* document = nullptr;
    rendering::path::CachedPathTexture texture;
};

// Convenience composition of load_asset_svg() + rendering::path::acquire_svg_texture(): one texture
// per asset/size/scale, inheriting PathTextureCache's bounded LRU, D2 format rule and D7 handling.
// The texture key uses the asset content hash as the document identity, so a changed asset is a new
// key automatically.
[[nodiscard]] std::expected<AssetSvgTexture, AssetSvgError>
load_asset_svg_texture(const AssetBundle& bundle, std::string_view name,
                       rendering::path::SvgDocumentCache& cache,
                       rendering::path::PathTextureCache& textures,
                       std::uint32_t scale_milli = 1000, rendering::path::SvgLimits limits = {});

} // namespace tessera::ui::assets
