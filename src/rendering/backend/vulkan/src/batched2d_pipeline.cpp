// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace {

alignas(4) const std::uint32_t kVertSpv[] =
#include "batched2d_vert_spv.h"
    ;

alignas(4) const std::uint32_t kFragSpv[] =
#include "batched2d_frag_spv.h"
    ;

// The LCD variety reuses batched2d.vert: the vertex interface is identical, only the fragment
// program and the blend state differ (issue #471 slice 1).
alignas(4) const std::uint32_t kLcdFragSpv[] =
#include "batched2d_lcd_frag_spv.h"
    ;

// The descriptor-indexing variety (RHI 1.7, issue #480 part 2): the same fragment source compiled
// with TESSERA_TEXTURE_ARRAY, so its clip/SDF/shadow logic cannot drift from the default variant.
alignas(4) const std::uint32_t kArrayFragSpv[] =
#include "batched2d_array_frag_spv.h"
    ;

} // namespace

namespace tessera::rendering::vulkan {
namespace {

// Fields both batched2d variants share. Keeping them in one place is the point of the variant: a
// run switches between grayscale and LCD by swapping the pipeline, not by changing vertex data or
// the push-constant block.
rhi::PipelineDesc batched2d_common_desc(rhi::TextureFormat color_format,
                                        std::uint32_t sample_count) {
    rhi::PipelineDesc desc;
    desc.vertex_spirv = kVertSpv;
    desc.vertex_spirv_size = sizeof(kVertSpv);
    desc.vertex_layout.stride = gpu::kVertex2DStride;
    desc.vertex_layout.attributes = gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = gpu::kVertex2DAttributeCount;
    desc.color_format = color_format;
    desc.sample_count = sample_count;
    desc.push_constant_size = sizeof(gpu::Batched2DPushConstants);
    return desc;
}

} // namespace

rhi::PipelineDesc batched2d_pipeline_desc(rhi::TextureFormat color_format,
                                          std::uint32_t sample_count) {
    rhi::PipelineDesc desc = batched2d_common_desc(color_format, sample_count);
    desc.fragment_spirv = kFragSpv;
    desc.fragment_spirv_size = sizeof(kFragSpv);
    desc.blend = rhi::BlendMode::Alpha;
    desc.shader_id = gpu::kBatched2dShaderId;
    return desc;
}

rhi::PipelineDesc batched2d_lcd_pipeline_desc(rhi::TextureFormat color_format,
                                              std::uint32_t sample_count) {
    rhi::PipelineDesc desc = batched2d_common_desc(color_format, sample_count);
    desc.fragment_spirv = kLcdFragSpv;
    desc.fragment_spirv_size = sizeof(kLcdFragSpv);
    desc.blend = rhi::BlendMode::DualSource;
    desc.shader_id = gpu::kBatched2dLcdShaderId;
    return desc;
}

rhi::PipelineDesc batched2d_array_pipeline_desc(rhi::TextureFormat color_format,
                                                std::uint32_t sample_count) {
    rhi::PipelineDesc desc = batched2d_common_desc(color_format, sample_count);
    desc.fragment_spirv = kArrayFragSpv;
    desc.fragment_spirv_size = sizeof(kArrayFragSpv);
    desc.blend = rhi::BlendMode::Alpha;
    desc.shader_id = gpu::kBatched2dArrayShaderId;
    desc.texture_array = true;
    return desc;
}

} // namespace tessera::rendering::vulkan
