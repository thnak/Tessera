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

} // namespace

namespace tessera::rendering::vulkan {

rhi::PipelineDesc batched2d_pipeline_desc(rhi::TextureFormat color_format,
                                          std::uint32_t sample_count) {
    rhi::PipelineDesc desc;
    desc.vertex_spirv = kVertSpv;
    desc.vertex_spirv_size = sizeof(kVertSpv);
    desc.fragment_spirv = kFragSpv;
    desc.fragment_spirv_size = sizeof(kFragSpv);
    desc.vertex_layout.stride = gpu::kVertex2DStride;
    desc.vertex_layout.attributes = gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = gpu::kVertex2DAttributeCount;
    desc.color_format = color_format;
    desc.sample_count = sample_count;
    desc.blend = rhi::BlendMode::Alpha;
    desc.push_constant_size = sizeof(gpu::Batched2DPushConstants);
    desc.shader_id = gpu::kBatched2dShaderId;
    return desc;
}

} // namespace tessera::rendering::vulkan
