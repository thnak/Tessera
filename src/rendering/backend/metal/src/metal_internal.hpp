// SPDX-License-Identifier: MIT

#pragma once

// Private to src/rendering/backend/metal/src/. Objective-C++ types are allowed here and nowhere
// else: no header outside this directory may see an `id<...>` or an `NS*` type
// (docs/design/gpu/metal-backend.md section 4).

#include "retire_queue.hpp"
#include "tessera/rendering/metal/metal_mapping.hpp"
#include "tessera/rendering/metal/metal_shader_registry.hpp"
#include "tessera/rendering/rhi/device.hpp"

#import <Metal/Metal.h>
#include <cstdint>
#include <memory>

namespace tessera::rendering::metal {

// The vertex-stage argument-table slot of the vertex stream. SPIRV-Cross puts the vertex shader's
// push-constant block at buffer(0), and the argument table is shared by buffers and bytes, so the
// stream lives at the top of the table (Metal allows 31 slots) instead of colliding with it.
inline constexpr std::uint32_t kVertexStreamIndex = 30;

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
    // A read-only storage buffer is a `[[buffer(n)]]` argument of the fragment function, which
    // every Metal device has (ADR-068 slice 2, docs/design/gpu/metal-backend.md section 4.2). The
    // pipeline-side requirement is that the program's MSL declares it; create_pipeline refuses a
    // `PipelineDesc::storage_buffer` request for a program that does not.
    [[nodiscard]] bool supports_storage_buffers() const override {
        return true;
    }
    // RHI 1.20 (ADR-076): supports_analytic_gradients() stays at its default (false). The branch is
    // in the cross-compiled batched2d MSL, but no Metal test renders a gradient quad, so the banded
    // fallback remains until one does (docs/design/gpu/gradients.md section 4).

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
    MetalBuffer(id<MTLBuffer> buffer, std::size_t size, rhi::BufferUsage usage);

    [[nodiscard]] std::size_t size() const override;
    void write(const void* data, std::size_t size, std::size_t offset) override;
    void read(void* data, std::size_t size, std::size_t offset) const override;

    [[nodiscard]] id<MTLBuffer> handle() const {
        return buffer_;
    }
    [[nodiscard]] rhi::BufferUsage usage() const {
        return usage_;
    }

private:
    void check_range(std::size_t size, std::size_t offset) const;

    id<MTLBuffer> buffer_;
    std::size_t size_;
    rhi::BufferUsage usage_;
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
                  std::uint32_t push_constant_size, std::uint32_t fragment_push_constant_index,
                  std::uint32_t fragment_storage_buffer_index, bool uses_vertex_stream);

    [[nodiscard]] std::uint32_t sample_count() const override;
    // True when the program's MSL declares the read-only storage buffer and the descriptor asked
    // for it (`PipelineDesc::storage_buffer`).
    [[nodiscard]] bool declares_storage_buffer() const override {
        return fragment_storage_buffer_index_ != kNoMslBuffer;
    }
    // The fragment `[[buffer(n)]]` slots of the push-constant block and the storage buffer.
    [[nodiscard]] std::uint32_t fragment_push_constant_index() const {
        return fragment_push_constant_index_;
    }
    [[nodiscard]] std::uint32_t fragment_storage_buffer_index() const {
        return fragment_storage_buffer_index_;
    }
    // False for a program that builds its geometry from `vertex_id` (a fullscreen triangle): a draw
    // with it needs no vertex buffer.
    [[nodiscard]] bool uses_vertex_stream() const {
        return uses_vertex_stream_;
    }
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
    std::uint32_t fragment_push_constant_index_;
    std::uint32_t fragment_storage_buffer_index_;
    bool uses_vertex_stream_;
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
