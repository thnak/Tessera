// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>

namespace tessera::rendering::vulkan {

// The GPU path rasterizer's four compute passes (ADR-071), one pipeline each. Every pass binds the
// same six storage buffers and the same push block (`path::PathRasterPush`), so a caller binds them
// once per pass and dispatches in this order, with a `compute_barrier()` between passes:
//
//   Clear   work words [0, nodes_base): the node counter and every (path, row) list head.
//           ceil(nodes_base / 256) groups.
//   Stroke  expands each stroke record into edges inside the work buffer. ceil(stroke_count / 256)
//           groups; skipped when the scene has no stroke.
//   Bin     pushes each segment onto the lists of the pixel rows it crosses. ceil(seg_count / 256)
//           groups.
//   Fine    one thread per pixel: exact-area coverage, fill rule, source-over into the output.
//           ceil(width / 16) x ceil(height / 16) groups.
//
// Storage buffer slots: 0 fill segments, 1 path records, 2 work (gpu_read_write), 3 output
// (gpu_read_write), 4 stroke records, 5 dash records. The layouts are
// `include/tessera/rendering/path/gpu_path_encoder.hpp`; the interface is the `path_raster` entry
// of `tools/shaders/manifest.json`. A device reports `supports_compute()` before this is created.
enum class PathRasterPass : std::uint8_t { Clear = 0, Stroke = 1, Bin = 2, Fine = 3 };

inline constexpr std::uint32_t kPathRasterStorageBuffers = 6;
inline constexpr std::uint32_t kPathRasterPushBytes = 48;

[[nodiscard]] rhi::ComputePipelineDesc path_raster_pipeline_desc(PathRasterPass pass);

} // namespace tessera::rendering::vulkan
