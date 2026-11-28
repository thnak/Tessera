// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.

#include "webgpu_internal.hpp"

#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tessera::rendering::webgpu {

WebGpuPipeline::WebGpuPipeline(std::shared_ptr<WebGpuContext> context,
                               const rhi::PipelineDesc& desc)
    : context_(std::move(context)),
      storage_buffer_(desc.storage_buffer),
      sample_count_(desc.sample_count),
      uniform_size_(push_constant_uniform_size(desc.push_constant_size)) {
    if (!is_supported_sample_count(desc.sample_count)) {
        throw std::invalid_argument("WebGpuPipeline: unsupported sample count");
    }
    if (validate_vertex_layout(desc.vertex_layout) != VertexLayoutError::None) {
        throw std::invalid_argument("WebGpuPipeline: invalid vertex layout");
    }
    // Reject dual-source before allocating anything: `blend_state()` would throw later, after the
    // shader module and the bind-group layouts exist, and a constructor that throws must not leak
    // them. WebGPU has no SRC1 blend factor and one fragment output per target, so the mode is
    // simply not expressible; the caller's documented fallback is grayscale coverage (issue #471).
    if (!blend_mode_supported(desc.blend)) {
        throw std::invalid_argument(
            "WebGpuPipeline: dual-source blending is not supported by WebGPU");
    }
    // The vertex and fragment stages share one WGSL module named by the manifest shader id; the
    // SPIR-V words are Vulkan's and are ignored here (ADR-049).
    const WgslProgram* program = context_->shaders.find(desc.shader_id);
    if (program == nullptr) {
        throw std::invalid_argument("WebGpuPipeline: empty or unknown shader_id");
    }

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = wgpu_string(program->source);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(context_->device, &module_desc);

    // Group 0: [0] push-constant uniform (dynamic offset per draw), [1] texture, [2] sampler, and
    // for a pipeline that declares one (ADR-068) [3] the read-only clip-record storage buffer. The
    // WGSL twin declares binding 3 unconditionally, so a layout without it is a validation error at
    // pipeline creation; the one shader that has it is created with `storage_buffer`.
    std::array<WGPUBindGroupLayoutEntry, 4> entries{};
    for (auto& entry : entries) {
        entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    }
    entries[0].binding = kBindingPushConstants;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[0].buffer.minBindingSize = uniform_size_;
    entries[1].binding = kBindingTexture;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = kBindingSampler;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[3].binding = kBindingClipRecords;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

    WGPUBindGroupLayoutDescriptor bgl_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.entryCount = storage_buffer_ ? entries.size() : entries.size() - 1;
    bgl_desc.entries = entries.data();
    bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(context_->device, &bgl_desc);

    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &bind_group_layout_;
    layout_ = wgpuDeviceCreatePipelineLayout(context_->device, &layout_desc);

    std::vector<WGPUVertexAttribute> attributes(desc.vertex_layout.attribute_count);
    for (std::uint32_t i = 0; i < attributes.size(); ++i) {
        const rhi::VertexAttribute& source = desc.vertex_layout.attributes[i];
        attributes[i].format = to_wgpu(vertex_format(source.format));
        attributes[i].offset = source.offset;
        attributes[i].shaderLocation = source.location;
    }
    WGPUVertexBufferLayout vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.arrayStride = desc.vertex_layout.stride;
    vertex_layout.attributeCount = attributes.size();
    vertex_layout.attributes = attributes.data();

    const WgpuBlendState blend = blend_state(desc.blend);
    WGPUBlendState wgpu_blend = WGPU_BLEND_STATE_INIT;
    wgpu_blend.color = {WGPUBlendOperation_Add, to_wgpu(blend.color.src), to_wgpu(blend.color.dst)};
    wgpu_blend.alpha = {WGPUBlendOperation_Add, to_wgpu(blend.alpha.src), to_wgpu(blend.alpha.dst)};
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = to_wgpu(texture_format(desc.color_format));
    target.blend = blend.enabled ? &wgpu_blend : nullptr;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = wgpu_string(program->fragment_entry);
    fragment.targetCount = 1;
    fragment.targets = &target;

    WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.layout = layout_;
    pipeline_desc.vertex.module = module;
    pipeline_desc.vertex.entryPoint = wgpu_string(program->vertex_entry);
    // The empty layout (a fullscreen triangle from the vertex index) binds no vertex buffer at all.
    pipeline_desc.vertex.bufferCount = attributes.empty() ? 0 : 1;
    pipeline_desc.vertex.buffers = attributes.empty() ? nullptr : &vertex_layout;
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.multisample.count = desc.sample_count;
    pipeline_desc.multisample.mask = 0xFFFFFFFFu;
    pipeline_desc.fragment = &fragment;
    pipeline_ = wgpuDeviceCreateRenderPipeline(context_->device, &pipeline_desc);
    wgpuShaderModuleRelease(module);
    if (pipeline_ == nullptr) {
        throw std::runtime_error("WebGPU: render pipeline creation failed");
    }
}

WebGpuPipeline::~WebGpuPipeline() {
    if (pipeline_ != nullptr) {
        wgpuRenderPipelineRelease(pipeline_);
    }
    if (layout_ != nullptr) {
        wgpuPipelineLayoutRelease(layout_);
    }
    if (bind_group_layout_ != nullptr) {
        wgpuBindGroupLayoutRelease(bind_group_layout_);
    }
}

} // namespace tessera::rendering::webgpu
