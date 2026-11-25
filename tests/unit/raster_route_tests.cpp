// SPDX-License-Identifier: MIT
//
// The CPU/GPU raster route policy (ADR-071): a pure function, no device involved.

#include "tessera/rendering/path/raster_route.hpp"

#include <iostream>
#include <string>

namespace {

namespace path = tessera::rendering::path;

bool g_ok = true;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "raster_route: " << message << "\n";
        g_ok = false;
    }
}

path::RasterRouteInput input(std::uint32_t side, std::uint64_t nodes = 1000, bool gpu = true) {
    return {side, side, nodes, gpu};
}

} // namespace

int main() {
    using path::RasterRoute;
    using path::RasterRouteReason;

    // Icons stay on ThorVG; the line is 128 x 128 pixels.
    expect(path::choose_raster_route(input(64)).reason == RasterRouteReason::too_small,
           "64 px is too small");
    expect(path::choose_raster_route(input(127)).route == RasterRoute::cpu, "127 px stays CPU");
    expect(path::choose_raster_route(input(128)).route == RasterRoute::gpu, "128 px goes GPU");
    expect(path::choose_raster_route({256, 64, 10, true}).route == RasterRoute::gpu,
           "the rule is on pixel count, not on one side (16384 px)");
    expect(path::choose_raster_route({100, 100, 10, true}).route == RasterRoute::cpu,
           "10000 px is below 16384");

    // Dense scenes fall back; the bound itself is allowed.
    expect(path::choose_raster_route(input(1024, 262'144)).route == RasterRoute::gpu,
           "the node bound is inclusive");
    const auto dense = path::choose_raster_route(input(1024, 262'145));
    expect(dense.route == RasterRoute::cpu && dense.reason == RasterRouteReason::too_dense,
           "over the node bound goes CPU");
    path::RasterRouteConfig unlimited;
    unlimited.max_row_nodes = 0;
    unlimited.max_nodes_per_kilopixel = 0;
    expect(path::choose_raster_route(input(1024, 10'000'000), unlimited).route == RasterRoute::gpu,
           "0 disables the density check");

    // Density: a filled icon (about 1000 nodes at 256 px) goes, a round-join stroked one (81000)
    // does not, and a 1 px bound shows the density check is what refuses it.
    expect(path::choose_raster_route(input(256, 1'789)).route == RasterRoute::gpu,
           "a filled icon is sparse");
    const auto stroked = path::choose_raster_route(input(256, 81'104));
    expect(stroked.route == RasterRoute::cpu && stroked.reason == RasterRouteReason::too_dense,
           "a round-join stroked icon is too dense");
    expect(path::row_node_bound(256, 256, {}) == 16'384, "256 px: 0.25 nodes per pixel");
    expect(path::row_node_bound(1024, 1024, {}) == 262'144, "the fixed bound is the tighter one");

    // No capability, no GPU, whatever the mode asks for (except it never reaches the GPU).
    path::RasterRouteConfig force_gpu;
    force_gpu.mode = path::RasterRouteMode::gpu_when_possible;
    expect(path::choose_raster_route(input(1024, 1000, false), force_gpu).reason ==
               RasterRouteReason::no_gpu_support,
           "capability check wins over force-GPU");
    const auto forced = path::choose_raster_route(input(16, 10'000'000), force_gpu);
    expect(forced.route == RasterRoute::gpu && forced.reason == RasterRouteReason::forced_gpu,
           "force-GPU skips size and density");

    path::RasterRouteConfig cpu_only;
    cpu_only.mode = path::RasterRouteMode::cpu_only;
    expect(path::choose_raster_route(input(4096), cpu_only).reason == RasterRouteReason::forced_cpu,
           "cpu_only always CPU");

    // Tunable line.
    path::RasterRouteConfig big;
    big.min_pixels = 1'000'000;
    expect(path::choose_raster_route(input(512), big).route == RasterRoute::cpu,
           "a raised line moves the cutoff");

    // Zero-size requests are CPU's problem (invalid_request there), never a GPU dispatch.
    expect(path::choose_raster_route({0, 4096, 0, true}).route == RasterRoute::cpu,
           "zero width is CPU");

    if (!g_ok) {
        return 1;
    }
    std::cout << "raster_route: ok\n";
    return 0;
}
