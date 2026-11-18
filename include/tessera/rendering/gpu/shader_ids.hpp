// SPDX-License-Identifier: MIT

#pragma once

// Logical shader ids carried in `rhi::PipelineDesc::shader_id` (ADR-049). One id names one shader
// program across every backend: Vulkan compiles it from GLSL, WebGPU embeds its WGSL twin, and a
// cross-compiled backend registers translator output under the same id. The ids are manifest names
// (docs/specs/shader-manifest-v1.md), so never rename one without a manifest migration.

#include <string_view>

namespace tessera::rendering::gpu {

inline constexpr std::string_view kBatched2dShaderId = "batched2d";
// Glass backdrop blur (docs/specs/glass-v1.md section 5). Both passes share the one id; the pass
// (horizontal blur vs vertical blur + composite) is a push-constant mode, not a second program.
inline constexpr std::string_view kGlassBlurShaderId = "glass_blur";
// LCD subpixel variant of the batched2d program (issue #471 slice 1,
// docs/design/display-aware-rendering.md section 3.4). It is Vulkan-only by contract: the manifest
// entry has no WebGPU source because WGSL has no fragment output index, so no backend resolves this
// id against a WGSL program. The value is the manifest name, per docs/specs/shader-manifest-v1.md.
inline constexpr std::string_view kBatched2dLcdShaderId = "batched2d_lcd";
// Descriptor-indexing variant of the batched2d program (RHI 1.7, issue #480 part 2): the same
// vertex interface and push-constant block, a fragment program that samples an array of combined
// image samplers indexed by the per-vertex texture index. Vulkan-only by construction: WebGPU has
// no descriptor indexing, so no backend resolves this id against a WGSL program.
inline constexpr std::string_view kBatched2dArrayShaderId = "batched2d_array";

} // namespace tessera::rendering::gpu
