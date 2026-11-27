// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp. The compute half of
// the WebGPU command list and the compute pipeline (RHI 1.22, issue #684,
// docs/design/gpu/compute.md).

#include "tessera/rendering/rhi/texture_region.hpp"
#include "webgpu_internal.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tessera::rendering::webgpu {
namespace {

// The same per-dimension limit the Vulkan and null backends validate against.
constexpr std::uint32_t kMaxDispatchCount = 65535;
// The same push-constant ceiling as Vulkan's guaranteed minimum.
constexpr std::uint32_t kMaxComputePushBytes = 128;
// One uniform slice per `compute_push_constants` call: 64 KiB per command list, like the draw
// arena.
constexpr std::uint32_t kComputeArenaSlices = 256;

} // namespace

WebGpuComputePipeline::WebGpuComputePipeline(std::shared_ptr<WebGpuContext> context,
                                             const rhi::ComputePipelineDesc& desc)
    : context_(std::move(context)),
      storage_buffer_count_(desc.storage_buffer_count),
      push_constant_size_(desc.push_constant_size) {
    if (desc.storage_buffer_count == 0 ||
        desc.storage_buffer_count > rhi::kMaxComputeStorageBuffers) {
        throw std::invalid_argument("create_compute_pipeline: storage_buffer_count " +
                                    std::to_string(desc.storage_buffer_count) + " is out of range");
    }
    if (desc.push_constant_size % 4 != 0 || desc.push_constant_size > kMaxComputePushBytes) {
        throw std::invalid_argument(
            "create_compute_pipeline: push_constant_size must be a multiple of 4 and at most 128");
    }
    // The SPIR-V words are Vulkan's; the WGSL comes from the registry by name (ADR-049).
    const WgslProgram* program = context_->shaders.find(desc.shader_id);
    if (program == nullptr) {
        throw std::invalid_argument("create_compute_pipeline: empty or unknown shader_id");
    }

    std::vector<WGPUBindGroupLayoutEntry> entries(desc.storage_buffer_count + 1);
    for (auto& entry : entries) {
        entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    }
    // Every slot is a read/write storage binding: the RHI binds one kind of buffer and the shader
    // declares `var<storage, read_write>` for all of them (WebGPU rejects a pipeline whose layout
    // and declared access disagree).
    for (std::uint32_t i = 0; i < desc.storage_buffer_count; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Compute;
        entries[i].buffer.type = WGPUBufferBindingType_Storage;
    }
    std::size_t entry_count = desc.storage_buffer_count;
    if (desc.push_constant_size > 0) {
        WGPUBindGroupLayoutEntry& push = entries[entry_count++];
        push.binding = kBindingComputePush;
        push.visibility = WGPUShaderStage_Compute;
        push.buffer.type = WGPUBufferBindingType_Uniform;
        push.buffer.hasDynamicOffset = true;
        push.buffer.minBindingSize = push_constant_uniform_size(desc.push_constant_size);
    }
    WGPUBindGroupLayoutDescriptor bgl_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.entryCount = entry_count;
    bgl_desc.entries = entries.data();
    bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(context_->device, &bgl_desc);

    WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &bind_group_layout_;
    layout_ = wgpuDeviceCreatePipelineLayout(context_->device, &layout_desc);

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = wgpu_string(program->source);
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module_desc.nextInChain = &wgsl.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(context_->device, &module_desc);

    WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.layout = layout_;
    pipeline_desc.compute.module = module;
    pipeline_desc.compute.entryPoint = wgpu_string(program->compute_entry);
    pipeline_ = wgpuDeviceCreateComputePipeline(context_->device, &pipeline_desc);
    wgpuShaderModuleRelease(module);
    if (pipeline_ == nullptr) {
        throw std::runtime_error("WebGPU: compute pipeline creation failed");
    }
}

WebGpuComputePipeline::~WebGpuComputePipeline() {
    if (pipeline_ != nullptr) {
        wgpuComputePipelineRelease(pipeline_);
    }
    if (layout_ != nullptr) {
        wgpuPipelineLayoutRelease(layout_);
    }
    if (bind_group_layout_ != nullptr) {
        wgpuBindGroupLayoutRelease(bind_group_layout_);
    }
}

WebGpuComputeCommands::WebGpuComputeCommands(std::shared_ptr<WebGpuContext> context)
    : compute_context_(std::move(context)) {}

WebGpuComputeCommands::~WebGpuComputeCommands() {
    end_compute_pass();
    if (compute_group_ != nullptr) {
        wgpuBindGroupRelease(compute_group_);
    }
    if (compute_arena_ != nullptr) {
        wgpuBufferDestroy(compute_arena_);
        wgpuBufferRelease(compute_arena_);
    }
}

void WebGpuComputeCommands::require_outside_render_pass(const char* call) const {
    if (render_pass_open()) {
        throw std::logic_error(std::string("WebGpuCommandList: ") + call +
                               " is recorded outside a render pass");
    }
}

void WebGpuComputeCommands::end_compute_pass() {
    if (compute_pass_ != nullptr) {
        wgpuComputePassEncoderEnd(compute_pass_);
        wgpuComputePassEncoderRelease(compute_pass_);
        compute_pass_ = nullptr;
    }
}

void WebGpuComputeCommands::bind_compute_pipeline(const rhi::ComputePipeline& pipeline) {
    require_outside_render_pass("bind_compute_pipeline");
    const auto* next = static_cast<const WebGpuComputePipeline*>(&pipeline);
    if (next != compute_pipeline_) {
        // The bindings belong to the pipeline, so a different one starts with none (Vulkan's
        // descriptor set is per pipeline object too).
        compute_pipeline_ = next;
        compute_buffers_ = {};
        compute_push_written_ = false;
        if (compute_group_ != nullptr) {
            wgpuBindGroupRelease(compute_group_);
            compute_group_ = nullptr;
        }
    }
    if (compute_pass_ != nullptr) {
        wgpuComputePassEncoderSetPipeline(compute_pass_, compute_pipeline_->handle());
    }
}

void WebGpuComputeCommands::bind_compute_buffer(std::uint32_t slot, const rhi::Buffer& buffer) {
    require_outside_render_pass("bind_compute_buffer");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("WebGpuCommandList: bind_compute_buffer needs a compute pipeline");
    }
    if (slot >= compute_pipeline_->storage_buffer_count()) {
        throw std::invalid_argument("bind_compute_buffer: slot " + std::to_string(slot) +
                                    " is outside the pipeline's storage buffers");
    }
    if (buffer.size() == 0) {
        throw std::invalid_argument("bind_compute_buffer: an empty buffer cannot be bound");
    }
    const auto& wgpu_buffer = static_cast<const WebGpuBuffer&>(buffer);
    if (wgpu_buffer.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "bind_compute_buffer: the buffer was not created with BufferUsage::Storage");
    }
    compute_buffers_[slot] = &wgpu_buffer;
    if (compute_group_ != nullptr) {
        wgpuBindGroupRelease(compute_group_);
        compute_group_ = nullptr;
    }
}

void WebGpuComputeCommands::compute_push_constants(const void* data, std::size_t size) {
    require_outside_render_pass("compute_push_constants");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error(
            "WebGpuCommandList: compute_push_constants needs a compute pipeline");
    }
    if (size == 0 || size > compute_pipeline_->push_constant_size() || size % 4 != 0) {
        throw std::invalid_argument("compute_push_constants: size " + std::to_string(size) +
                                    " does not fit the pipeline's push-constant block");
    }
    if (compute_arena_ == nullptr) {
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.usage = buffer_usage_bits::kUniform | buffer_usage_bits::kCopyDst;
        desc.size = static_cast<std::uint64_t>(kUniformSliceAlignment) * kComputeArenaSlices;
        compute_arena_ = wgpuDeviceCreateBuffer(compute_context_->device, &desc);
    }
    const std::uint32_t stride = uniform_slice_stride(compute_pipeline_->push_constant_size());
    if (compute_next_slice_ >= kComputeArenaSlices) {
        throw std::length_error("WebGpuCommandList: compute push-constant arena exhausted");
    }
    compute_push_offset_ = compute_next_slice_ * kUniformSliceAlignment;
    compute_next_slice_ += stride / kUniformSliceAlignment;
    // The binding is the aligned block size, so the tail past `size` is written as zeros.
    std::array<std::uint8_t, kMaxComputePushBytes> padded{};
    std::memcpy(padded.data(), data, size);
    wgpuQueueWriteBuffer(compute_context_->queue, compute_arena_, compute_push_offset_,
                         padded.data(),
                         push_constant_uniform_size(compute_pipeline_->push_constant_size()));
    compute_push_written_ = true;
}

// Opens the pass if needed and binds the group with the current push-constant slice. A pass that
// ended since the last dispatch (a barrier, a copy, a render pass) is reopened with the pipeline
// set.
void WebGpuComputeCommands::begin_dispatch() {
    require_outside_render_pass("dispatch");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("WebGpuCommandList: dispatch needs a compute pipeline");
    }
    for (std::uint32_t i = 0; i < compute_pipeline_->storage_buffer_count(); ++i) {
        if (compute_buffers_[i] == nullptr) {
            throw std::logic_error("WebGpuCommandList: compute storage slot " + std::to_string(i) +
                                   " is not bound");
        }
    }
    if (compute_pipeline_->push_constant_size() > 0 && !compute_push_written_) {
        // A pipeline that declares push constants but was never given any reads zeros, not
        // whatever an earlier pipeline left in the arena.
        const std::array<std::uint8_t, kMaxComputePushBytes> zeros{};
        compute_push_constants(zeros.data(), compute_pipeline_->push_constant_size());
    }
    if (compute_pass_ == nullptr) {
        WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        compute_pass_ = wgpuCommandEncoderBeginComputePass(compute_encoder(), &desc);
        wgpuComputePassEncoderSetPipeline(compute_pass_, compute_pipeline_->handle());
    }
    if (compute_group_ == nullptr) {
        std::vector<WGPUBindGroupEntry> entries(compute_pipeline_->storage_buffer_count() + 1);
        for (auto& entry : entries) {
            entry = WGPU_BIND_GROUP_ENTRY_INIT;
        }
        for (std::uint32_t i = 0; i < compute_pipeline_->storage_buffer_count(); ++i) {
            entries[i].binding = i;
            entries[i].buffer = compute_buffers_[i]->handle();
            entries[i].size = WGPU_WHOLE_SIZE;
        }
        std::size_t entry_count = compute_pipeline_->storage_buffer_count();
        if (compute_pipeline_->push_constant_size() > 0) {
            WGPUBindGroupEntry& push = entries[entry_count++];
            push.binding = kBindingComputePush;
            push.buffer = compute_arena_;
            push.size = push_constant_uniform_size(compute_pipeline_->push_constant_size());
        }
        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.layout = compute_pipeline_->bind_group_layout();
        desc.entryCount = entry_count;
        desc.entries = entries.data();
        compute_group_ = wgpuDeviceCreateBindGroup(compute_context_->device, &desc);
    }
    if (compute_pipeline_->push_constant_size() > 0) {
        wgpuComputePassEncoderSetBindGroup(compute_pass_, 0, compute_group_, 1,
                                           &compute_push_offset_);
    } else {
        wgpuComputePassEncoderSetBindGroup(compute_pass_, 0, compute_group_, 0, nullptr);
    }
}

void WebGpuComputeCommands::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (x == 0 || y == 0 || z == 0 || x > kMaxDispatchCount || y > kMaxDispatchCount ||
        z > kMaxDispatchCount) {
        throw std::invalid_argument("dispatch: workgroup counts must be 1.." +
                                    std::to_string(kMaxDispatchCount));
    }
    begin_dispatch();
    wgpuComputePassEncoderDispatchWorkgroups(compute_pass_, x, y, z);
}

void WebGpuComputeCommands::dispatch_indirect(const rhi::Buffer& buffer, std::size_t offset) {
    const auto& arguments = static_cast<const WebGpuBuffer&>(buffer);
    if (arguments.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "dispatch_indirect: the buffer was not created with BufferUsage::Storage");
    }
    constexpr std::size_t kArgumentBytes = 3 * sizeof(std::uint32_t);
    if (offset % 4 != 0 || offset > buffer.size() || buffer.size() - offset < kArgumentBytes) {
        throw std::invalid_argument("dispatch_indirect: offset " + std::to_string(offset) +
                                    " is not 4-byte aligned with 12 bytes inside the buffer");
    }
    begin_dispatch();
    wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute_pass_, arguments.handle(), offset);
}

void WebGpuComputeCommands::compute_barrier() {
    require_outside_render_pass("compute_barrier");
    end_compute_pass();
}

void WebGpuComputeCommands::copy_buffer(const rhi::Buffer& source, rhi::Buffer& destination,
                                        std::size_t size) {
    require_outside_render_pass("copy_buffer");
    if (size == 0 || size > source.size() || size > destination.size()) {
        throw std::invalid_argument("copy_buffer: size " + std::to_string(size) +
                                    " does not fit both buffers");
    }
    // A buffer-to-buffer copy moves whole words; the RHI size is a byte count, so reject a tail the
    // API cannot express instead of copying less than asked.
    if (size % 4 != 0) {
        throw std::invalid_argument("copy_buffer: size must be a multiple of 4 on WebGPU");
    }
    end_compute_pass();
    wgpuCommandEncoderCopyBufferToBuffer(compute_encoder(),
                                         static_cast<const WebGpuBuffer&>(source).handle(), 0,
                                         static_cast<WebGpuBuffer&>(destination).handle(), 0, size);
}

// RHI 1.17 (ADR-072): the handoff from a compute pass's storage buffer to a texture a later draw
// samples. The RHI pitch is already a multiple of 256 bytes (`checked_buffer_texture_copy`), which
// is WebGPU's own bytesPerRow rule, so the region maps one to one. The copy is its own encoder
// command, so it ends the compute pass and is ordered after the dispatches recorded before it.
void WebGpuComputeCommands::copy_buffer_to_texture(const rhi::Buffer& source,
                                                   rhi::Texture& destination,
                                                   const rhi::BufferTextureCopy& copy) {
    require_outside_render_pass("copy_buffer_to_texture");
    const auto& from = static_cast<const WebGpuBuffer&>(source);
    auto& to = static_cast<WebGpuTexture&>(destination);
    if (from.usage() != rhi::BufferUsage::Staging && from.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "copy_buffer_to_texture: the source must be a Staging or Storage buffer");
    }
    if (to.sample_count() != 1 || to.levels() != 1) {
        throw std::invalid_argument(
            "copy_buffer_to_texture: the destination must have one sample and one level");
    }
    const std::size_t pitch =
        rhi::checked_buffer_texture_copy(to.extent(), to.format(), copy, from.size());
    if (pitch == 0) {
        return; // an empty region copies nothing
    }
    end_compute_pass();
    WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    src.buffer = from.handle();
    src.layout.offset = copy.buffer_offset;
    src.layout.bytesPerRow = static_cast<std::uint32_t>(pitch);
    src.layout.rowsPerImage = copy.region.height;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = to.handle();
    dst.origin = {copy.region.x, copy.region.y, 0};
    const WGPUExtent3D extent{copy.region.width, copy.region.height, 1};
    wgpuCommandEncoderCopyBufferToTexture(compute_encoder(), &src, &dst, &extent);
}

} // namespace tessera::rendering::webgpu
