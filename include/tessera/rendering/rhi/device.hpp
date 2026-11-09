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

    // Both throw std::out_of_range when [offset, offset + size) does not lie inside the buffer,
    // and std::runtime_error if the memory cannot be mapped.
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

    // Synchronous submit: returns once the GPU has finished. For offscreen rendering, readback
    // and tests only; frame code on the UI thread uses submit_async().
    virtual void submit() = 0;

    // Queues the list and returns without waiting. The caller must keep every resource the list
    // references (buffers, textures, pipelines, targets) alive until is_complete() or wait()
    // reports completion. Destroying the list earlier is safe: the device recycles its command
    // resources only after the GPU is done with them.
    virtual void submit_async() = 0;

    // Non-blocking poll. False until the list has been submitted and the GPU has finished it.
    [[nodiscard]] virtual bool is_complete() const = 0;

    // Waits at most `timeout_ms` for completion; returns whether the list is complete. A zero
    // timeout polls.
    [[nodiscard]] virtual bool wait(std::uint64_t timeout_ms) = 0;
};

// Device health for the D7 production policy. A loss is recoverable backend state, not an
// exception: operations that can report it (swapchain acquire/present, command `wait()`) return an
// error/false and the caller polls `status()`. After a loss every RHI object created before it is
// invalid and must be destroyed and recreated (the backend generation-guards destructors so a late
// destruction is safe); the device itself can be recreated with `recover()`.
enum class DeviceStatus { Ready, Lost };

enum class DeviceRecoveryError {
    // The backend has no recovery path (null/software backends).
    NotSupported,
    // `recover()` was called while the device is healthy.
    NotLost,
    // This loss was already recovered once and failed; a new loss is needed (one-shot policy).
    AlreadyAttempted,
    // RHI objects from the lost generation are still alive. Destroy them, then retry; this attempt
    // does not consume the one-shot budget.
    ResourcesOutstanding,
    // Device recreation failed (driver/device unavailable). The device stays Lost.
    DeviceCreationFailed,
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

    // Current health. Losses are detected by the backend on submit/acquire/present/fence waits and
    // are recorded here; a caller that sees `Lost` must stop using the device and recover.
    [[nodiscard]] virtual DeviceStatus status() const {
        return DeviceStatus::Ready;
    }

    // Bounded, one-shot recovery of a lost device (device + backend-owned state only; caller-owned
    // RHI objects are invalid and must be recreated by the caller). Returns a value, never throws.
    // On success `status()` is Ready and the caller recreates its resources and swapchain. On
    // failure the device stays Lost. Non-Vulkan backends report NotSupported.
    [[nodiscard]] virtual std::expected<void, DeviceRecoveryError> recover() {
        return std::unexpected(DeviceRecoveryError::NotSupported);
    }
};

} // namespace tessera::rendering::rhi
