// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <cstdint>

namespace tessera::rendering::path {

// Identity of a cached path raster. A raster is a pure function of these fields plus the geometry
// the caller submits with the request, so equal keys may share one texture. Floats are not part of
// the key: callers quantize scale, stroke width and miter limit to thousandths, which keeps the key
// exact and stops float-equality surprises from causing permanent cache misses.
struct PathFillKey {
    bool present = false;
    std::uint32_t rgba = 0; // R | G << 8 | B << 16 | A << 24 (layout-independent packing)

    [[nodiscard]] bool operator==(const PathFillKey&) const noexcept = default;
};

struct PathStrokeKey {
    bool present = false;
    std::uint32_t rgba = 0;
    std::uint32_t width_milli = 0; // stroke width x1000
    std::uint32_t miter_milli = 0; // miter limit x1000
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;

    [[nodiscard]] bool operator==(const PathStrokeKey&) const noexcept = default;
};

struct PathTextureKey {
    // Caller identity of the geometry (scene node id, asset id, ...). `path_version` must be bumped
    // whenever the geometry changes; the cache cannot detect a geometry change by itself.
    std::uint64_t path_id = 0;
    std::uint32_t path_version = 0;
    // Logical box the texture is drawn into, in device pixels.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Raster scale x1000 (1000 = 1x, 2000 = 2x). The texture is rasterized at
    // `width * scale` by `height * scale` pixels and minified back to the logical box when drawn.
    std::uint32_t scale_milli = 1000;
    FillRule fill_rule = FillRule::non_zero;
    PathFillKey fill;
    PathStrokeKey stroke;

    [[nodiscard]] bool operator==(const PathTextureKey&) const noexcept = default;
};

// Builds the full key for `shape`, packing the fill rule, fill color/presence and stroke signature
// (color, width, cap, join, miter limit). Every field that changes pixels is represented, so a
// caller that reuses one id for a restyled path cannot read a stale texture.
[[nodiscard]] PathTextureKey make_path_texture_key(std::uint64_t path_id,
                                                   std::uint32_t path_version, std::uint32_t width,
                                                   std::uint32_t height, std::uint32_t scale_milli,
                                                   const PathShape& shape) noexcept;

} // namespace tessera::rendering::path
