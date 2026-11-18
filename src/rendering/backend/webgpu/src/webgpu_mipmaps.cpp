// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.
//
// Mip chain generation for 2D textures (RHI 1.6, issue #510). WebGPU has no blit, so each level is
// drawn from the one above with a fullscreen triangle and a linear sampler (shaders/
// mip_downsample.wgsl). It runs once at texture creation, on its own submission, after the caller's
// base level was written to the queue, so the chain is complete before the texture's first draw.

#include "webgpu_internal.hpp"

#include <stdexcept>

namespace tessera::rendering::webgpu {
namespace {

// The pipeline for one render-target format, built on first use and kept by the context.
const WebGpuContext::MipPipeline& mip_pipeline(WebGpuContext& context, WGPUTextureFormat format) {
    for (const WebGpuContext::MipPipeline& cached : context.mip_pipelines) {
        if (cached.format == format) {
            return cached;
        }
    }

    const WgslProgram program = mip_downsample_wgsl_program();
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = wgpu_string(program.source);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(context.device, &module_desc);

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = module;
    fragment.entryPoint = wgpu_string(program.fragment_entry);
    fragment.targetCount = 1;
    fragment.targets = &target;

    // No explicit layout: the bind group layout (texture + filtering sampler) is derived from the
    // shader and read back with wgpuRenderPipelineGetBindGroupLayout.
    WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.vertex.module = module;
    pipeline_desc.vertex.entryPoint = wgpu_string(program.vertex_entry);
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = 0xFFFFFFFFu;
    pipeline_desc.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(context.device, &pipeline_desc);
    wgpuShaderModuleRelease(module);
    if (pipeline == nullptr) {
        throw std::runtime_error("WebGPU: mip downsample pipeline creation failed");
    }
    context.mip_pipelines.push_back({format, pipeline});
    return context.mip_pipelines.back();
}

} // namespace

void generate_mip_chain(WebGpuContext& context, WGPUTexture texture, rhi::TextureFormat format,
                        std::uint32_t levels) {
    const WebGpuContext::MipPipeline& downsample =
        mip_pipeline(context, to_wgpu(texture_format(format)));

    if (context.mip_sampler == nullptr) {
        WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler_desc.magFilter = WGPUFilterMode_Linear;
        sampler_desc.minFilter = WGPUFilterMode_Linear;
        sampler_desc.addressModeU = WGPUAddressMode_ClampToEdge;
        sampler_desc.addressModeV = WGPUAddressMode_ClampToEdge;
        sampler_desc.addressModeW = WGPUAddressMode_ClampToEdge;
        context.mip_sampler = wgpuDeviceCreateSampler(context.device, &sampler_desc);
    }

    WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(downsample.pipeline, 0);
    WGPUCommandEncoderDescriptor encoder_desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(context.device, &encoder_desc);

    for (std::uint32_t level = 1; level < levels; ++level) {
        // One view per level: the source is level N-1 alone, the attachment level N alone.
        WGPUTextureViewDescriptor source_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        source_desc.baseMipLevel = level - 1;
        source_desc.mipLevelCount = 1;
        WGPUTextureView source = wgpuTextureCreateView(texture, &source_desc);
        WGPUTextureViewDescriptor target_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        target_desc.baseMipLevel = level;
        target_desc.mipLevelCount = 1;
        WGPUTextureView target = wgpuTextureCreateView(texture, &target_desc);

        WGPUBindGroupEntry entries[2];
        entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].textureView = source;
        entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[1].binding = 1;
        entries[1].sampler = context.mip_sampler;
        WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        group_desc.layout = layout;
        group_desc.entryCount = 2;
        group_desc.entries = entries;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(context.device, &group_desc);

        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = target;
        // Every texel of the level is written, so nothing needs loading.
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        wgpuRenderPassEncoderSetPipeline(pass, downsample.pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);

        // The recorded pass keeps its own references to the view, bind group and pipeline.
        wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(group);
        wgpuTextureViewRelease(target);
        wgpuTextureViewRelease(source);
    }

    WGPUCommandBufferDescriptor buffer_desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, &buffer_desc);
    wgpuQueueSubmit(context.queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuBindGroupLayoutRelease(layout);
}

} // namespace tessera::rendering::webgpu
