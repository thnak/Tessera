// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <span>
#include <vector>

namespace tessera::rendering::path {

// Returns a copy of `shapes` scaled by `scale` for rasterization at a higher resolution: every
// segment point and every stroke width materialises at the scaled size, so drawing the raster back
// down into the logical box restores the authored geometry and stroke width. When `scale` is 1 the
// result is empty (callers keep the original span instead of copying). Shared by the texture cache
// (GPU upload) and CPU-side sinks so the scaling rule cannot drift.
[[nodiscard]] std::vector<PathShape> scale_path_shapes(std::span<const PathShape> shapes,
                                                       float scale);

} // namespace tessera::rendering::path
