// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.

#include "webgpu_internal.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace tessera::rendering::webgpu {
namespace {

// Largest push-constant block the RHI uses is 112 bytes (Batched2DPushConstants); one slice each.
constexpr std::uint32_t kMaxPushConstantBytes = 256;
constexpr std::uint32_t kArenaSlices = 256; // 64 KiB per command list

struct Completion {
    bool done = false;
};

void on_work_done(WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata, void*) {
    auto* state = static_cast<std::shared_ptr<Completion>*>(userdata);
    (*state)->done = true;
    delete state;
}

class WebGpuCommandList final : public rhi::CommandList {
public:
    explicit WebGpuCommandList(std::shared_ptr<WebGpuContext> context)
        : context_(std::move(context)),
          completion_(std::make_shared<Completion>()) {
        WGPUCommandEncoderDescriptor desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
        encoder_ = wgpuDeviceCreateCommandEncoder(context_->device, &desc);
    }

    ~WebGpuCommandList() override {
        release_pass();
        if (encoder_ != nullptr) {
            wgpuCommandEncoderRelease(encoder_);
        }
        if (arena_ != nullptr) {
            wgpuBufferDestroy(arena_);
            wgpuBufferRelease(arena_);
        }
    }

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override {
        if (pass_ != nullptr) {
            throw std::logic_error("WebGpuCommandList: render pass already open");
        }
        const auto& wgpu_target = static_cast<const WebGpuRenderTarget&>(target);
        target_extent_ = target.extent();
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = wgpu_target.color_view();
        color.resolveTarget = wgpu_target.resolve_view();
        color.loadOp = to_wgpu(load_op(desc.load_op));
        color.storeOp = to_wgpu(store_op(desc.store_op));
        color.clearValue = {desc.clear_color.r, desc.clear_color.g, desc.clear_color.b,
                            desc.clear_color.a};
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &pass_desc);
    }

    void end_render_pass() override {
        if (pass_ == nullptr) {
            throw std::logic_error("WebGpuCommandList: no open render pass");
        }
        wgpuRenderPassEncoderEnd(pass_);
        release_pass();
    }

    void bind_pipeline(const rhi::Pipeline& pipeline) override {
        pipeline_ = &static_cast<const WebGpuPipeline&>(pipeline);
        wgpuRenderPassEncoderSetPipeline(require_pass(), pipeline_->handle());
        dirty_ = true;
    }

    void bind_vertex_buffer(const rhi::Buffer& buffer) override {
        const auto& wgpu_buffer = static_cast<const WebGpuBuffer&>(buffer);
        wgpuRenderPassEncoderSetVertexBuffer(require_pass(), 0, wgpu_buffer.handle(), 0,
                                             WGPU_WHOLE_SIZE);
    }

    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override {
        if (slot != 0) {
            throw std::invalid_argument("WebGpuCommandList: only texture slot 0 exists");
        }
        texture_ = &static_cast<const WebGpuTexture&>(texture);
        dirty_ = true;
    }

    void set_viewport(const rhi::Viewport& viewport) override {
        wgpuRenderPassEncoderSetViewport(require_pass(), viewport.x, viewport.y, viewport.width,
                                         viewport.height, viewport.min_depth, viewport.max_depth);
    }

    void set_scissor(const rhi::Scissor& scissor) override {
        const rhi::Scissor clamped = clamp_scissor(scissor, target_extent_);
        wgpuRenderPassEncoderSetScissorRect(require_pass(), static_cast<std::uint32_t>(clamped.x),
                                            static_cast<std::uint32_t>(clamped.y), clamped.width,
                                            clamped.height);
    }

    void push_constants(const void* data, std::size_t size) override {
        if (size > kMaxPushConstantBytes) {
            throw std::out_of_range("WebGpuCommandList: push-constant block too large");
        }
        std::memcpy(push_.data(), data, size);
        push_size_ = static_cast<std::uint32_t>(size);
        dirty_ = true;
    }

    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex) override {
        WGPURenderPassEncoder pass = require_pass();
        if (pipeline_ == nullptr || texture_ == nullptr) {
            throw std::logic_error("WebGpuCommandList: draw needs a pipeline and a texture");
        }
        if (dirty_) {
            flush_bind_group(pass);
        }
        wgpuRenderPassEncoderDraw(pass, vertex_count, 1, first_vertex, 0);
    }

    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override {
        const auto& wgpu_target = static_cast<const WebGpuRenderTarget&>(target);
        auto& wgpu_buffer = static_cast<WebGpuBuffer&>(buffer);
        const rhi::Extent2D extent = target.extent();
        if (buffer.size() < readback_buffer_size(extent, target.format())) {
            throw std::out_of_range("WebGpuCommandList: readback buffer is too small");
        }
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = wgpu_target.resolved_texture();
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = wgpu_buffer.handle();
        dst.layout.bytesPerRow = padded_bytes_per_row(extent.width, target.format());
        dst.layout.rowsPerImage = extent.height;
        const WGPUExtent3D size{extent.width, extent.height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder_, &src, &dst, &size);
    }

    // A browser cannot block on the GPU; use submit_async() and poll is_complete().
    void submit() override {
        throw std::logic_error(
            "WebGpuCommandList: synchronous submit is unavailable in the browser");
    }

    void submit_async() override {
        if (pass_ != nullptr) {
            throw std::logic_error("WebGpuCommandList: render pass still open");
        }
        WGPUCommandBufferDescriptor desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder_, &desc);
        wgpuQueueSubmit(context_->queue, 1, &commands);
        wgpuCommandBufferRelease(commands);

        WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = on_work_done;
        info.userdata1 = new std::shared_ptr<Completion>(completion_);
        wgpuQueueOnSubmittedWorkDone(context_->queue, info);
    }

    [[nodiscard]] bool is_complete() const override {
        return completion_->done;
    }

    // Single-threaded Wasm cannot spin for the callback, so a wait is a poll.
    [[nodiscard]] bool wait(std::uint64_t) override {
        return completion_->done;
    }

private:
    WGPURenderPassEncoder require_pass() const {
        if (pass_ == nullptr) {
            throw std::logic_error("WebGpuCommandList: no open render pass");
        }
        return pass_;
    }

    void release_pass() {
        if (pass_ != nullptr) {
            wgpuRenderPassEncoderRelease(pass_);
            pass_ = nullptr;
        }
        pipeline_ = nullptr;
        texture_ = nullptr;
        dirty_ = false;
    }

    void flush_bind_group(WGPURenderPassEncoder pass) {
        const std::uint32_t stride = uniform_slice_stride(pipeline_->uniform_size());
        if (arena_ == nullptr) {
            WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
            desc.usage = buffer_usage_bits::kUniform | buffer_usage_bits::kCopyDst;
            desc.size = static_cast<std::uint64_t>(kUniformSliceAlignment) * kArenaSlices;
            arena_ = wgpuDeviceCreateBuffer(context_->device, &desc);
        }
        if (next_slice_ >= kArenaSlices) {
            throw std::length_error("WebGpuCommandList: uniform arena exhausted");
        }
        const std::uint32_t offset = next_slice_ * kUniformSliceAlignment;
        next_slice_ += stride / kUniformSliceAlignment;
        std::array<std::uint8_t, kMaxPushConstantBytes> padded{};
        std::memcpy(padded.data(), push_.data(), push_size_);
        wgpuQueueWriteBuffer(context_->queue, arena_, offset, padded.data(),
                             pipeline_->uniform_size());

        std::array<WGPUBindGroupEntry, 3> entries{};
        for (auto& entry : entries) {
            entry = WGPU_BIND_GROUP_ENTRY_INIT;
        }
        entries[0].binding = kBindingPushConstants;
        entries[0].buffer = arena_;
        entries[0].size = pipeline_->uniform_size();
        entries[1].binding = kBindingTexture;
        entries[1].textureView = texture_->view();
        entries[2].binding = kBindingSampler;
        entries[2].sampler = texture_->sampler();
        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.layout = pipeline_->bind_group_layout();
        desc.entryCount = entries.size();
        desc.entries = entries.data();
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(context_->device, &desc);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 1, &offset);
        // The pass keeps the group alive until submit.
        wgpuBindGroupRelease(group);
        dirty_ = false;
    }

    std::shared_ptr<WebGpuContext> context_;
    std::shared_ptr<Completion> completion_;
    WGPUCommandEncoder encoder_ = nullptr;
    WGPURenderPassEncoder pass_ = nullptr;
    WGPUBuffer arena_ = nullptr;
    std::uint32_t next_slice_ = 0;
    const WebGpuPipeline* pipeline_ = nullptr;
    const WebGpuTexture* texture_ = nullptr;
    std::array<std::uint8_t, kMaxPushConstantBytes> push_{};
    std::uint32_t push_size_ = 0;
    rhi::Extent2D target_extent_;
    bool dirty_ = false;
};

} // namespace

std::unique_ptr<rhi::CommandList> make_command_list(std::shared_ptr<WebGpuContext> context) {
    return std::make_unique<WebGpuCommandList>(std::move(context));
}

} // namespace tessera::rendering::webgpu
