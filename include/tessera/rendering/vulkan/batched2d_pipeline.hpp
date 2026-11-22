// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include <cstdint>

namespace tessera::rendering::vulkan {

// Pipeline identity for batched2d: (color format, sample count). Cache callers key on both; the
// sample count must match the render target used with the pipeline.
rhi::PipelineDesc
batched2d_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm,
                        std::uint32_t sample_count = 1);

// Pipeline identity for the LCD subpixel variant of the batched2d program (issue #471 slice 1,
// docs/design/display-aware-rendering.md section 3.4): the same vertex layout and push-constant
// block as `batched2d_pipeline_desc`, its fragment shader (`batched2d_lcd.frag`) writes per-channel
// coverage to a second output, and `blend` is `BlendMode::DualSource` rather than Alpha.
//
// The window creates this pipeline when the device supports dual-source blending, and hands it to
// `Batched2DRenderer::set_lcd_pipeline`. `create_pipeline` rejects it on a device without
// `dualSrcBlend`, so the caller must check `Device::supports_dual_source_blend()` first
// (tests/gpu/rhi/dual_source_blend_tests.cpp proves the contract on a real device).
[[nodiscard]] rhi::PipelineDesc
batched2d_lcd_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm,
                            std::uint32_t sample_count = 1);

// Pipeline identity for the descriptor-indexing variant of the batched2d program (RHI 1.7, issue
// #480 part 2): the same vertex layout and push-constant block, a fragment shader that samples an
// array of combined image samplers indexed by `Vertex2D::texture_index`, and a pipeline layout that
// declares that array (`PipelineDesc::texture_array`).
//
// The window creates this pipeline when `Device::texture_array_capacity()` is non-zero, and hands
// it to `Batched2DRenderer::set_texture_array_pipeline`. `create_pipeline` rejects it on a device
// without descriptor indexing, so the caller must check the capability first.
[[nodiscard]] rhi::PipelineDesc
batched2d_array_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm,
                              std::uint32_t sample_count = 1);

} // namespace tessera::rendering::vulkan
