// SPDX-License-Identifier: MIT

#pragma once

// The GPU path rasterizer's four compute passes (ADR-071) on the WebGPU backend: the WebGPU twin of
// `vulkan::path_raster_pipeline_descs()`
// (include/tessera/rendering/vulkan/path_raster_pipeline.hpp). One WGSL module
// (shaders/path_raster.wgsl) carries the four entry points; each pass is its own `WgslProgram`
// under its own shader id, because a compute pipeline names its program by id and the program
// carries the entry point (docs/design/gpu/compute.md section 3.1). The buffer slots, the push
// block (4 bytes, `job_base`) and the dispatch order are the Vulkan header's; here the push block
// is the uniform at binding 8 (`kBindingComputePush`) and every storage buffer is declared
// read_write.
//
//   webgpu::register_path_raster_shaders(*device);          // once, at startup
//   path::GpuPathRasterizer gpu(*device, webgpu::path_raster_pipeline_descs());

#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::rendering::webgpu {

enum class PathRasterPass : std::uint8_t { Clear = 0, Stroke = 1, Bin = 2, Fine = 3 };

inline constexpr std::uint32_t kPathRasterStorageBuffers = 7;
inline constexpr std::uint32_t kPathRasterPushBytes = 4; // job_base

// "path_raster_clear", "path_raster_stroke", "path_raster_bin", "path_raster_fine".
[[nodiscard]] std::string_view path_raster_shader_id(PathRasterPass pass);

// The program of `pass`: the shared WGSL source with that pass's entry point.
[[nodiscard]] WgslProgram path_raster_wgsl_program(PathRasterPass pass);

// The descriptor of `pass`: `shader_id` set, seven storage buffers, a 4-byte push block.
[[nodiscard]] rhi::ComputePipelineDesc path_raster_pipeline_desc(PathRasterPass pass);

// The four passes in the form `path::GpuPathRasterizer` takes.
[[nodiscard]] path::GpuPathPipelineDescs path_raster_pipeline_descs();

// Registers the four programs with `device` (a WebGPU device). False if the device refused one.
// Needs the device half of the backend (Emscripten or TESSERA_BUILD_WEBGPU).
bool register_path_raster_shaders(rhi::Device& device);

} // namespace tessera::rendering::webgpu
