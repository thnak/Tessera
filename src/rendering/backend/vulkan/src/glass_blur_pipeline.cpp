// SPDX-License-Identifier: MIT
//
// The glass backdrop blur pipeline (docs/specs/glass-v1.md section 5, issue #244): a fullscreen
// triangle that reads one texture and writes one color target, so the same pipeline records the
// horizontal blur into a pooled target and the vertical blur plus composite into the frame target.
//
// What it reads is a *sampled render target* since RHI 1.3 (`CommandList::bind_sampled_target`),
// which is why this descriptor lives behind the same public header as `batched2d_pipeline_desc`:
// the composite test builds it and proves the two-pass blur end to end on a real device
// (tests/gpu/batched2d/glass_composite_gpu_tests.cpp).

#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace {

alignas(4) const std::uint32_t kVertSpv[] =
#include "fullscreen_vert_spv.h"
    ;

alignas(4) const std::uint32_t kFragSpv[] =
#include "glass_blur_frag_spv.h"
    ;

} // namespace

namespace tessera::rendering::vulkan {

rhi::PipelineDesc glass_blur_pipeline_desc(rhi::TextureFormat color_format) {
    rhi::PipelineDesc desc;
    desc.vertex_spirv = kVertSpv;
    desc.vertex_spirv_size = sizeof(kVertSpv);
    desc.fragment_spirv = kFragSpv;
    desc.fragment_spirv_size = sizeof(kFragSpv);
    // fullscreen.vert builds its triangle from gl_VertexIndex: no vertex buffer, no attributes.
    desc.vertex_layout = rhi::VertexBufferLayout{};
    desc.color_format = color_format;
    // The blur writes the frame target: alpha blending would let the backdrop show through the
    // blurred result, so the composite is a straight overwrite of the region it covers.
    desc.blend = rhi::BlendMode::None;
    desc.push_constant_size = sizeof(gpu::GlassPushConstants);
    desc.shader_id = gpu::kGlassBlurShaderId;
    return desc;
}

} // namespace tessera::rendering::vulkan
