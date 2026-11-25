// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::path {

// Which rasterizer fills a path texture: the ThorVG CPU raster or the GPU compute rasterizer
// (ADR-071). A pure decision so it is testable without a device; the caller supplies what the
// device and the encoded scene say.
enum class RasterRoute : std::uint8_t {
    cpu = 0,
    gpu,
};

enum class RasterRouteReason : std::uint8_t {
    forced_cpu = 0,    // config says CPU only
    forced_gpu,        // config says GPU whenever it is possible
    no_gpu_support,    // the device cannot copy a buffer into a texture, or has no compute
    too_small,         // below `min_pixels`: a dispatch costs more than the raster it replaces
    too_dense,         // more bin nodes than `max_row_nodes`: the fine pass loses to the CPU
    gpu_large_surface, // large enough that the CPU raster is the cost
};

enum class RasterRouteMode : std::uint8_t {
    automatic = 0, // size and density rules below
    cpu_only,
    gpu_when_possible, // skips the size and density rules, keeps the capability check
};

struct RasterRouteConfig {
    RasterRouteMode mode = RasterRouteMode::automatic;
    // 128 x 128. Measured on 2026-11-25 (RX 5300M, iGPU; SVG route at 32/64/256/1024 px): up to
    // 64 px the GPU is no faster than ThorVG once the encode and the wait count (~0.1 ms fixed),
    // from 256 px it wins. The line sits between, so icons stay on ThorVG.
    std::uint64_t min_pixels = 128ULL * 128ULL;
    // Upper bound on the encoder's `row_nodes` (bin nodes at scale 1). The iGPU measurement:
    // 145k nodes ran 10.1 ms against ThorVG's 11.1 ms (break-even), 360k ran 34 ms against 23 ms.
    // 0 disables the check.
    std::uint64_t max_row_nodes = 262'144;
};

// What the device and the encoded scene say about one raster request.
struct RasterRouteInput {
    std::uint32_t width = 0; // target pixels
    std::uint32_t height = 0;
    std::uint64_t row_nodes = 0;             // EncodedGpuScene::row_nodes
    bool device_supports_gpu_raster = false; // supports_buffer_texture_copy() and compute
};

struct RasterRouteDecision {
    RasterRoute route = RasterRoute::cpu;
    RasterRouteReason reason = RasterRouteReason::forced_cpu;
};

[[nodiscard]] RasterRouteDecision choose_raster_route(const RasterRouteInput& input,
                                                      const RasterRouteConfig& config = {});

} // namespace tessera::rendering::path
