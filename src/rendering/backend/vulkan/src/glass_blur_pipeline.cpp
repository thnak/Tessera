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
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"

#include <algorithm>
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

rhi::PipelineDesc glass_blur_pipeline_desc(rhi::TextureFormat color_format, rhi::BlendMode blend) {
    rhi::PipelineDesc desc;
    desc.vertex_spirv = kVertSpv;
    desc.vertex_spirv_size = sizeof(kVertSpv);
    desc.fragment_spirv = kFragSpv;
    desc.fragment_spirv_size = sizeof(kFragSpv);
    // fullscreen.vert builds its triangle from gl_VertexIndex: no vertex buffer, no attributes.
    desc.vertex_layout = rhi::VertexBufferLayout{};
    desc.color_format = color_format;
    // The horizontal pass writes straight-alpha blur data into a pooled target (no blending); the
    // composite pass draws over the frame target, where the shader's alpha is the blend weight.
    desc.blend = blend;
    // The push-constant range is the batched2d block's size, not the glass block's. Vulkan requires
    // two pipeline layouts to be *fully* compatible for a descriptor-set bind recorded against one
    // to stay valid under the other, and the push-constant range is part of that comparison. The
    // renderer records a set-0 bind for the frame and then switches to the glass pipeline mid-frame
    // (ADR-068 slice 3 made the batched2d block 16 bytes), so the two ranges must agree. The glass
    // shader reads only its own 112 bytes, which fit inside the larger range.
    desc.push_constant_size = static_cast<std::uint32_t>(
        std::max(sizeof(gpu::GlassPushConstants), sizeof(gpu::Batched2DPushConstants)));
    desc.shader_id = gpu::kGlassBlurShaderId;
    // The glass passes run between the batched2d passes of the same frame, and the renderer binds a
    // sampled target at set 0 for both. Declaring the clip-record set here too keeps every pipeline
    // the renderer uses on one descriptor-set layout, so a set-0 bind stays compatible across the
    // pipeline switch. The glass shader does not read set 1; an unused declared set is legal and
    // costs nothing.
    desc.storage_buffer = true;
    return desc;
}

} // namespace tessera::rendering::vulkan
