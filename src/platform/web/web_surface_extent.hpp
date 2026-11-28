// SPDX-License-Identifier: MIT

#pragma once

#include "web_host.hpp"

#include <cstdint>

// The canvas backing store the web app driver renders into (issue #1013). The page reports the
// canvas in CSS pixels and a device pixel ratio; the surface, the render target and the host glue's
// `canvas.width/height` must agree on one integer size, so the rule lives here once instead of in
// the JS and the C++ separately.

namespace tessera::hal::web {

struct WebSurfaceExtent {
    std::uint32_t width = 1;
    std::uint32_t height = 1;

    [[nodiscard]] friend bool operator==(const WebSurfaceExtent&,
                                         const WebSurfaceExtent&) = default;
};

// WebGPU's guaranteed `maxTextureDimension2D`: a larger canvas would fail surface configuration, so
// the backing store is clamped (the page then upscales it, which only blurs).
inline constexpr std::uint32_t kWebMaxSurfaceDimension = 8192;

// round(css * dpr) per axis, at least 1 and at most `max_dimension`. A non-finite or non-positive
// size or ratio degrades to 1 (a hidden or zero-sized canvas) instead of an invalid surface.
[[nodiscard]] WebSurfaceExtent
backing_extent(const WebSurfaceMetrics& metrics,
               std::uint32_t max_dimension = kWebMaxSurfaceDimension);

} // namespace tessera::hal::web
