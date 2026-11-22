// SPDX-License-Identifier: MIT
//
// The storage-buffer conformance probe pipeline (RHI 1.12, ADR-068 slice 1): a fullscreen triangle
// whose fragment stage reads one read-only storage buffer and writes the value it found. It is a
// test fixture, not a production pipeline: it exists so `CommandList::bind_storage_buffer` is
// proven end to end on a real device, which is what the ADR's slice 1 acceptance asks for.

#include "tessera/rendering/vulkan/storage_probe_pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace {

alignas(4) const std::uint32_t kVertSpv[] =
#include "fullscreen_vert_spv.h"
    ;

alignas(4) const std::uint32_t kFragSpv[] =
#include "storage_probe_frag_spv.h"
    ;

} // namespace

namespace tessera::rendering::vulkan {

rhi::PipelineDesc storage_probe_pipeline_desc(rhi::TextureFormat color_format) {
    rhi::PipelineDesc desc;
    desc.vertex_spirv = kVertSpv;
    desc.vertex_spirv_size = sizeof(kVertSpv);
    desc.fragment_spirv = kFragSpv;
    desc.fragment_spirv_size = sizeof(kFragSpv);
    // fullscreen.vert builds its triangle from gl_VertexIndex: no vertex buffer, no attributes.
    desc.vertex_layout = rhi::VertexBufferLayout{};
    desc.color_format = color_format;
    // No blending: the probe writes the buffer's colour straight into the target, so a readback
    // compares exact bytes.
    desc.blend = rhi::BlendMode::None;
    desc.push_constant_size = 0;
    // The layout declares the read-only storage buffer at set 0, binding 0.
    desc.storage_buffer = true;
    return desc;
}

} // namespace tessera::rendering::vulkan
