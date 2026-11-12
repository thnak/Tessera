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
    // 1 or 4 (MSAA storage resource); multisampled textures cannot be sampled by the 2D pipeline.
    [[nodiscard]] virtual std::uint32_t sample_count() const {
        return 1;
    }

    // Uploads one tightly packed image (offset 0, extent.width * extent.height * bytes-per-pixel).
    // Dynamic atlas updates use this instead of recreating the GPU resource. Only valid for
    // sample_count == 1.
    virtual void write(const void* data, std::size_t size) = 0;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;

    // Sample count the pipeline was created for; it must equal the target's sample count.
    [[nodiscard]] virtual std::uint32_t sample_count() const {
        return 1;
    }
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;

    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;
    // 1 or 4. A 4x target owns a multisample render image plus a 1-sample resolve image; readback
    // and sampling observe the resolved image.
    [[nodiscard]] virtual std::uint32_t sample_count() const {
        return 1;
    }

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

    // Resamples `source` into `destination`, stretching to cover the destination's whole extent
    // with `filter`. This is the seam that lets a frame render into a smaller target and present it
    // at the window's physical size (adaptive render scale, docs/design/performance.md section 6).
    // `source` must be a rendered, non-presentable target; `destination` may be a swapchain image,
    // in which case the submission that records the blit also carries the acquire wait and the
    // present semaphore exactly like a render pass into that image would. Both targets must be
    // 1-sample and format-compatible (the same format in practice). A backend without a filtered
    // blit throws here; consumers that present through a swapchain ask
    // Swapchain::supports_scaled_present() first, so that call never reaches a frame.
    virtual void blit_target(const RenderTarget& source, const RenderTarget& destination,
                             Filter filter) = 0;

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

// GPU memory the device's allocator holds (issue #13). `block_bytes` is what was requested from the
// driver; `allocation_bytes` is the part handed to live buffers and images, so the gap is allocator
// slack. `buffer_bytes` and `image_bytes` split the live resources by kind: buffers are exact,
// images are estimated as extent x texel size (x samples, plus the resolve image, for multisampled
// targets), and presentation images are not counted because the driver owns them. All zero on
// backends that do not track it.
struct DeviceMemoryStats {
    std::uint64_t buffer_bytes = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t allocation_bytes = 0;
    std::uint32_t block_count = 0;
    std::uint32_t allocation_count = 0;
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

    // Snapshot of the allocator's totals. Cheap (no GPU work) but not free: sample at checkpoints,
    // not per frame.
    [[nodiscard]] virtual DeviceMemoryStats memory_statistics() const {
        return {};
    }

    // True when `sample_count` (1 or 4) is supported for `format` as a color render target on this
    // device. Unsupported requests must be reported to the caller (creation throws); backends never
    // silently fall back to 1-sample. The default implementation supports 1 sample only.
    [[nodiscard]] virtual bool supports_sample_count(TextureFormat /*format*/,
                                                     std::uint32_t sample_count) const {
        return sample_count == 1;
    }

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
