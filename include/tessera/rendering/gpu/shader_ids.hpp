// SPDX-License-Identifier: MIT

#pragma once

// Logical shader ids carried in `rhi::PipelineDesc::shader_id` (ADR-049). One id names one shader
// program across every backend: Vulkan compiles it from GLSL, WebGPU embeds its WGSL twin, and a
// cross-compiled backend registers translator output under the same id. The ids are manifest names
// (docs/specs/shader-manifest-v1.md), so never rename one without a manifest migration.

#include <string_view>

namespace tessera::rendering::gpu {

inline constexpr std::string_view kBatched2dShaderId = "batched2d";

} // namespace tessera::rendering::gpu
