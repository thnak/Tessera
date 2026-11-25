// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/raster_route.hpp"

#include <algorithm>

namespace tessera::rendering::path {

std::uint64_t row_node_bound(std::uint32_t width, std::uint32_t height,
                             const RasterRouteConfig& config) {
    std::uint64_t bound = config.max_row_nodes;
    if (config.max_nodes_per_kilopixel != 0) {
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        const std::uint64_t density = pixels * config.max_nodes_per_kilopixel / 1000U;
        bound = bound == 0 ? density : std::min(bound, density);
    }
    return bound;
}

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
    const std::uint64_t bound = row_node_bound(input.width, input.height, config);
    if (bound != 0 && input.row_nodes > bound) {
        return {RasterRoute::cpu, RasterRouteReason::too_dense};
    }
    return {RasterRoute::gpu, RasterRouteReason::gpu_large_surface};
}

} // namespace tessera::rendering::path
