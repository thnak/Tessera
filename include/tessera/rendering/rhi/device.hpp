// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

namespace tessera::rendering::rhi {

class Buffer {
public:
    virtual ~Buffer() = default;

    [[nodiscard]] virtual std::size_t size() const = 0;
    virtual void write(const void* data, std::size_t size, std::size_t offset = 0) = 0;
    virtual void read(void* data, std::size_t size, std::size_t offset = 0) const = 0;
};

class Texture {
public:
    virtual ~Texture() = default;

    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;

    // Uploads one tightly packed image (offset 0, extent.width * extent.height * bytes-per-pixel).
    // Dynamic atlas updates use this instead of recreating the GPU resource.
    virtual void write(const void* data, std::size_t size) = 0;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;

    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;

    // True for swapchain images, which must end in PRESENT_SRC instead of a transfer layout.
    [[nodiscard]] virtual bool presentable() const {
        return false;
    }
};

class CommandList {
public:
    virtual ~CommandList() = default;

    virtual void begin_render_pass(const RenderTarget& target, const RenderTargetDesc& desc) = 0;
    virtual void end_render_pass() = 0;
    virtual void bind_pipeline(const Pipeline& pipeline) = 0;
    virtual void bind_vertex_buffer(const Buffer& buffer) = 0;
    virtual void bind_texture(std::uint32_t slot, const Texture& texture) = 0;
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Scissor& scissor) = 0;
    virtual void push_constants(const void* data, std::size_t size) = 0;
    virtual void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) = 0;
    virtual void copy_target_to_buffer(const RenderTarget& target, Buffer& buffer) = 0;
    virtual void submit() = 0;
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Backend backend() const = 0;
    virtual std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> create_texture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> create_pipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<RenderTarget> create_render_target(const RenderTargetDesc& desc) = 0;
    virtual std::unique_ptr<CommandList> create_command_list() = 0;

    // Creates a presentation swapchain. Invalid descriptors (image_count < 2) and backends
    // without a surface path report NotSupported rather than throwing or returning nullptr.
    [[nodiscard]] virtual std::expected<std::unique_ptr<Swapchain>, SwapchainError>
    create_swapchain(const SwapchainDesc& desc) = 0;

    virtual void wait_idle() = 0;
};

} // namespace tessera::rendering::rhi
