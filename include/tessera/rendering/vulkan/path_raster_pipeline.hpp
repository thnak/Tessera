// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>

namespace tessera::rendering::vulkan {

// The GPU path rasterizer's four compute passes (ADR-071), one pipeline each. Every pass binds the
// same seven storage buffers and a 4-byte push block (`job_base`), so a caller binds them once per
// pass and dispatches in this order, with a `compute_barrier()` between passes. The jobs buffer
// holds one `path::PathRasterJob` per raster, and the job is the grid's second dimension (the fine
// pass: third), so N rasters are one dispatch per pass; the group counts below use the largest
// raster:
//
//   Clear   work words [0, 1 + path_count * height): the node counter and every (path, row) list
//           head. ceil(max words / 256) x N groups.
//   Stroke  expands each stroke record into edges inside the work buffer. ceil(max stroke_count /
//           256) x N groups; skipped when no raster of the batch has a stroke.
//   Bin     pushes each segment onto the lists of the pixel rows it crosses. ceil(max seg_count /
//           256) x N groups.
//   Fine    one thread per pixel: exact-area coverage, fill rule, source-over into the output.
//           ceil(max width / 16) x ceil(max height / 16) x N groups.
//
// A thread past its own raster's size returns at once. Storage buffer slots: 0 fill segments,
// 1 path records, 2 work (gpu_read_write), 3 output (gpu_read_write), 4 stroke records, 5 dash
// records, 6 jobs. The layouts are `include/tessera/rendering/path/gpu_path_encoder.hpp`; the
// interface is the `path_raster` entry of `tools/shaders/manifest.json`. A device reports
// `supports_compute()` before this is created.
enum class PathRasterPass : std::uint8_t { Clear = 0, Stroke = 1, Bin = 2, Fine = 3 };

inline constexpr std::uint32_t kPathRasterStorageBuffers = 7;
inline constexpr std::uint32_t kPathRasterPushBytes = 4; // job_base

[[nodiscard]] rhi::ComputePipelineDesc path_raster_pipeline_desc(PathRasterPass pass);

} // namespace tessera::rendering::vulkan
