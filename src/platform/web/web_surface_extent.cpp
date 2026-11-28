// SPDX-License-Identifier: MIT

#include "web_surface_extent.hpp"

#include <cmath>

namespace tessera::hal::web {
namespace {

std::uint32_t axis(float css, float ratio, std::uint32_t max_dimension) {
    if (!std::isfinite(css) || css <= 0.0F) {
        return 1;
    }
    const double pixels = std::round(static_cast<double>(css) * static_cast<double>(ratio));
    if (!(pixels >= 1.0)) {
        return 1;
    }
    if (pixels >= static_cast<double>(max_dimension)) {
        return max_dimension;
    }
    return static_cast<std::uint32_t>(pixels);
}

} // namespace

WebSurfaceExtent backing_extent(const WebSurfaceMetrics& metrics, std::uint32_t max_dimension) {
    const float ratio =
        std::isfinite(metrics.device_pixel_ratio) && metrics.device_pixel_ratio > 0.0F
            ? metrics.device_pixel_ratio
            : 1.0F;
    return {axis(metrics.width, ratio, max_dimension), axis(metrics.height, ratio, max_dimension)};
}

} // namespace tessera::hal::web
