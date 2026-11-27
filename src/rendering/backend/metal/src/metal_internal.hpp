// SPDX-License-Identifier: MIT

#pragma once

// Private to src/rendering/backend/metal/src/. Objective-C++ types are allowed here and nowhere
// else: no header outside this directory may see an `id<...>` or an `NS*` type
// (docs/design/gpu/metal-backend.md section 4).

#include "retire_queue.hpp"
#include "tessera/rendering/metal/metal_mapping.hpp"
#include "tessera/rendering/rhi/device.hpp"

#import <Metal/Metal.h>
#include <cstdint>
#include <memory>

namespace tessera::rendering::metal {

// The device, as the command-list, pipeline and swapchain translation units see it. The definition
// lives in metal_device.mm; this declaration is the seam between them, so each file can stay one
// responsibility without a public header carrying Metal types.
class MetalDevice final : public rhi::Device {
public:
    MetalDevice(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLSharedEvent> event,
                const rhi::DeviceDesc& desc);

    [[nodiscard]] rhi::Backend backend() const override;
    [[nodiscard]] rhi::RhiCapabilities capabilities() const override;

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override;
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override;
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override;
    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override;
    std::unique_ptr<rhi::CommandList> create_command_list() override;

    [[nodiscard]] std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override;

    void wait_idle() override;

    [[nodiscard]] bool supports_sample_count(rhi::TextureFormat format,
                                             std::uint32_t sample_count) const override;
    [[nodiscard]] bool supports_dual_source_blend() const override;
    [[nodiscard]] bool supports_texture_mipmaps(rhi::TextureFormat format) const override;
    // Metal has `[[buffer(n)]]` slots on the fragment stage, so a read-only storage buffer is
    // expressible; the binding itself is not implemented yet (ADR-068 slice 2). Reporting false
    // keeps a caller on the push-constant clip path instead of building a pipeline whose binding
    // can never be satisfied. Flip this to true in the same change that implements
    // bind_storage_buffer.
    [[nodiscard]] bool supports_storage_buffers() const override {
        return false;
    }
    // RHI 1.20 (ADR-076): the gradient branch reads the record buffer, so Metal keeps the banded
    // fallback until it answers true above (the default of supports_analytic_gradients() is false).

    [[nodiscard]] id<MTLDevice> device() const {
        return device_;
    }
    [[nodiscard]] id<MTLCommandQueue> queue() const {
        return queue_;
    }
    [[nodiscard]] id<MTLSharedEvent> event() const {
        return event_;
    }
    [[nodiscard]] const rhi::DeviceDesc& desc() const {
        return desc_;
    }

    // The next completion value for a submission. Values are handed out in commit order and never
    // reused, so a submission that never reaches the queue leaves a gap rather than a collision
    // (docs/design/gpu/metal-backend.md section 3.1).
    [[nodiscard]] std::uint64_t next_completion_value();
    // Publishes a completed value from a command buffer's completed handler.
    void publish_completed(std::uint64_t value);

    [[nodiscard]] rhi::RetireQueue& retire_queue() {
        return retire_;
    }
    [[nodiscard]] const rhi::CompletionSource& completion_source() const;

    // Sticky device loss (section 6).
    void mark_lost();
    [[nodiscard]] rhi::DeviceStatus status() const override {
        return status_;
    }

private:
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    id<MTLSharedEvent> event_;
    rhi::DeviceDesc desc_;
    std::uint64_t next_value_ = 0;
    std::unique_ptr<rhi::CompletionSource> completion_;
    rhi::RetireQueue retire_;
    rhi::DeviceStatus status_ = rhi::DeviceStatus::Ready;
};

// The Metal objects behind an RHI handle. A command list casts to these; the RHI contract says
// mixing objects from different backends is a programming error, so the casts are unchecked in
// release builds (docs/design/gpu/rhi.md section 6).
class MetalBuffer final : public rhi::Buffer {
public:
    MetalBuffer(id<MTLBuffer> buffer, std::size_t size);

    [[nodiscard]] std::size_t size() const override;
    void write(const void* data, std::size_t size, std::size_t offset) override;
    void read(void* data, std::size_t size, std::size_t offset) const override;

    [[nodiscard]] id<MTLBuffer> handle() const {
        return buffer_;
    }

private:
    void check_range(std::size_t size, std::size_t offset) const;

    id<MTLBuffer> buffer_;
    std::size_t size_;
};

class MetalTexture final : public rhi::Texture {
public:
    MetalTexture(id<MTLTexture> texture, rhi::Extent2D extent, rhi::TextureFormat format,
                 std::uint32_t levels);

    [[nodiscard]] rhi::Extent2D extent() const override;
    [[nodiscard]] rhi::TextureFormat format() const override;
    [[nodiscard]] std::uint32_t levels() const override;
    void write(const void* data, std::size_t size) override;
    void write_region(const rhi::TextureRegion& region, const void* data,
                      std::size_t row_pitch) override;

    [[nodiscard]] id<MTLTexture> handle() const {
        return texture_;
    }

private:
    id<MTLTexture> texture_;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_;
    std::uint32_t levels_;
};

// The pipeline a command list binds. Created by metal_pipeline.mm; the command list reads the
// Metal state object and the two recorded facts it needs.
class MetalPipeline final : public rhi::Pipeline {
public:
    MetalPipeline(id<MTLRenderPipelineState> state, std::uint32_t sample_count,
                  std::uint32_t push_constant_size);

    [[nodiscard]] std::uint32_t sample_count() const override;
    [[nodiscard]] id<MTLRenderPipelineState> state() const {
        return state_;
    }
    [[nodiscard]] std::uint32_t push_constant_size() const {
        return push_constant_size_;
    }

private:
    id<MTLRenderPipelineState> state_;
    std::uint32_t sample_count_;
    std::uint32_t push_constant_size_;
};

class MetalRenderTarget final : public rhi::RenderTarget {
public:
    MetalRenderTarget(id<MTLTexture> color, id<MTLTexture> resolve, rhi::Extent2D extent,
                      rhi::TextureFormat format, std::uint32_t sample_count, bool presentable,
                      rhi::Filter filter, rhi::AddressMode address);

    [[nodiscard]] rhi::Extent2D extent() const override;
    [[nodiscard]] rhi::TextureFormat format() const override;
    [[nodiscard]] std::uint32_t sample_count() const override;
    [[nodiscard]] bool presentable() const override;
    [[nodiscard]] rhi::Filter filter() const override;
    [[nodiscard]] rhi::AddressMode address() const override;

    // The attachment a render pass writes: the multisample image for a 4x target.
    [[nodiscard]] id<MTLTexture> attachment() const {
        return color_;
    }
    // The 1-sample image readback, sampling and copies observe.
    [[nodiscard]] id<MTLTexture> readable() const {
        return resolve_ != nil ? resolve_ : color_;
    }

private:
    id<MTLTexture> color_;
    id<MTLTexture> resolve_;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_;
    std::uint32_t sample_count_;
    bool presentable_;
    rhi::Filter filter_;
    rhi::AddressMode address_;
};

} // namespace tessera::rendering::metal
