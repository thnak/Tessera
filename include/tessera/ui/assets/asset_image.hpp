// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace tessera::ui::assets {

// Failure modes of loading an embedded raster image; every one is a value, never an exception.
enum class AssetImageError : std::uint8_t {
    not_found = 0,      // no such asset name (the documented missing-asset outcome)
    invalid_name,       // the requested name is not normalized
    input_too_large,    // encoded bytes exceed the decode limit
    unsupported_format, // not PNG/JPEG (or a 16-bit PNG)
    malformed,          // the decoder rejected the bytes
    limit_exceeded,     // decoded width/height/pixel budget exceeded
};

// Loads an embedded raster image by name through the decode cache: one stb_image decode per content
// hash, no filesystem access, deterministic RGBA8 straight-alpha output. The returned decode
// carries its content hash, which is the texture-cache key.
[[nodiscard]] std::expected<std::shared_ptr<const rendering::image::DecodedImage>, AssetImageError>
load_asset_image(const AssetBundle& bundle, std::string_view name,
                 rendering::image::ImageDecodeCache& cache,
                 rendering::image::ImageLimits limits = {});

} // namespace tessera::ui::assets
