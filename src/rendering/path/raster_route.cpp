// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/raster_route.hpp"

namespace tessera::rendering::path {

RasterRouteDecision choose_raster_route(const RasterRouteInput& input,
                                        const RasterRouteConfig& config) {
    if (config.mode == RasterRouteMode::cpu_only) {
        return {RasterRoute::cpu, RasterRouteReason::forced_cpu};
    }
    if (!input.device_supports_gpu_raster) {
        return {RasterRoute::cpu, RasterRouteReason::no_gpu_support};
    }
    if (config.mode == RasterRouteMode::gpu_when_possible) {
        return {RasterRoute::gpu, RasterRouteReason::forced_gpu};
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels < config.min_pixels) {
        return {RasterRoute::cpu, RasterRouteReason::too_small};
    }
    if (config.max_row_nodes != 0 && input.row_nodes > config.max_row_nodes) {
        return {RasterRoute::cpu, RasterRouteReason::too_dense};
    }
    return {RasterRoute::gpu, RasterRouteReason::gpu_large_surface};
}

} // namespace tessera::rendering::path
