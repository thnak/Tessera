// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/swapchain.hpp"
#include "tessera/rendering/rhi/texture_region.hpp"
#include "tessera/rendering/rhi/timestamp.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/rhi/version.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <stdexcept>
#include <vector>

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
    // Mip levels the texture was created with (RHI 1.6); 1 unless `TextureDesc::levels` asked more.
    [[nodiscard]] virtual std::uint32_t levels() const {
        return 1;
    }

    // Uploads one tightly packed image (offset 0, extent.width * extent.height * bytes-per-pixel).
    // Dynamic atlas updates use this instead of recreating the GPU resource. Only valid for
    // sample_count == 1.
    virtual void write(const void* data, std::size_t size) = 0;

    // Uploads one rectangle (RHI 1.9). `data` points at the region's first texel; `row_pitch` is
    // the byte distance between two source rows (0 = tightly packed). Validation is
    // `checked_region_pitch` (texture_region.hpp): a region outside the extent throws
    // std::out_of_range, a short pitch or a null `data` std::invalid_argument, an empty region
    // writes nothing. Only valid for sample_count == 1 and levels == 1, like `write`. It exists so
    // a producer that changed a few texels (one glyph in an atlas page) uploads those and not the
    // whole image. Pure virtual because a backend that cannot honour it would silently re-upload
    // everything and hide the cost the call exists to remove.
    virtual void write_region(const TextureRegion& region, const void* data,
                              std::size_t row_pitch) = 0;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;

    // Sample count the pipeline was created for; it must equal the target's sample count.
    [[nodiscard]] virtual std::uint32_t sample_count() const {
        return 1;
    }

    // True when the pipeline's layout declares the read-only storage buffer set
    // (`PipelineDesc::storage_buffer`, RHI 1.12, ADR-068). A caller that binds a storage buffer
    // must ask this first: binding one to a pipeline that does not declare it is a validation
    // error, not a no-op, and the renderer's clip-record path has to know whether the pipeline it
    // is about to draw with reads records at all.
    [[nodiscard]] virtual bool declares_storage_buffer() const {
        return false;
    }
};

// A compiled compute pipeline plus the one descriptor set its buffers are bound through (RHI 1.16).
class ComputePipeline {
public:
    virtual ~ComputePipeline() = default;
    [[nodiscard]] virtual std::uint32_t storage_buffer_count() const = 0;
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

    // Sampler state the target is read with when `CommandList::bind_sampled_target` binds it as a
    // texture (RHI 1.3). A target is created with it (`RenderTargetDesc::filter`/`address`), so the
    // bind itself allocates nothing; the defaults are linear filtering with clamp-to-edge
    // addressing. Every backend reports the state its target was created with.
    [[nodiscard]] virtual Filter filter() const {
        return Filter::Linear;
    }
    [[nodiscard]] virtual AddressMode address() const {
        return AddressMode::ClampToEdge;
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

    // Binds `textures` as one array of sampled images at descriptor `slot`, so a shader can index
    // them from a per-vertex value instead of one descriptor set per texture (RHI 1.7, issue #480
    // part 2). This is the seam that lets the batched renderer keep one draw when consecutive quads
    // sample different textures.
    //
    // Contract:
    //  - `textures.size()` must be at most `Device::texture_array_capacity()`; a longer array
    //  throws
    //    std::invalid_argument. An empty array is a no-op.
    //  - Every texture must be a 1-sample texture created by this device. The array is a snapshot:
    //    the caller re-binds it when the set of textures changes, which the renderer does at a
    //    batch boundary, never per quad.
    //  - A pipeline must have been bound, and its layout must declare a sampled-image array at
    //    `slot` with at least `textures.size()` descriptors.
    //  - Backends without the capability (`Device::texture_array_capacity() == 0`) throw
    //    std::runtime_error; callers ask the capability first, so this never reaches a frame.
    virtual void bind_texture_array(std::uint32_t /*slot*/,
                                    const std::vector<const Texture*>& /*textures*/) {
        throw std::runtime_error("bind_texture_array: this backend has no texture array "
                                 "(texture_array_capacity() is 0)");
    }

    // Binds `buffer` as a read-only storage buffer at descriptor `slot`, so a shader can read a
    // variable-length record list instead of a fixed push-constant block (RHI 1.12, ADR-068). This
    // is the seam `clip-path` needs: a clip set is a list of records whose length depends on the
    // scene, and a push constant cannot hold it.
    //
    // Contract:
    //  - `buffer` must have been created with `BufferUsage::Storage` by this device, and must not
    //  be
    //    empty; a zero-size buffer throws std::invalid_argument, because a shader that indexes it
    //    would read past its end.
    //  - A pipeline must have been bound, and its layout must declare a read-only storage buffer at
    //    `slot` (`PipelineDesc::storage_buffer`). Binding without one throws std::logic_error.
    //  - The storage buffer is a *separate descriptor set* from the sampled image, so a shader can
    //    sample a texture and read the record list in the same draw. `slot` names that set: a
    //    backend declares which one it is, and a slot the layout does not declare throws
    //    std::invalid_argument. Vulkan and null use set 1 (set 0 is the sampled image).
    //  - Backends without the capability (`Device::supports_storage_buffers() == false`) throw
    //    std::runtime_error; callers ask the capability first, so this never reaches a frame.
    //  - The buffer's contents are read at draw time, not at bind time, so a caller may rewrite it
    //    between draws. It must not be rewritten while a submission that reads it is in flight; the
    //    per-frame-buffer pattern (one buffer per frame in flight) is what keeps that true.
    virtual void bind_storage_buffer(std::uint32_t /*slot*/, const Buffer& /*buffer*/) {
        throw std::runtime_error("bind_storage_buffer: this backend has no storage buffers "
                                 "(supports_storage_buffers() is false)");
    }

    // Binds `target` as a texture at descriptor `slot`, so a shader can read pixels a render target
    // already holds. This is the RHI 1.3 sampled view of an offscreen target: the missing half of
    // the isolated-layer seam (RHI 1.1 `copy_target_region`) and the capability the glass composite
    // draws with (docs/design/gpu/offscreen-layers.md section 5.3, issue #254).
    //
    // Contract:
    //  - `target` must be a non-presentable target that holds rendered content (a preceding render
    //    pass, `copy_target_region` or `blit_target`), exactly like `copy_target_to_buffer`'s
    //    source. A 4x target is read through its resolved image, which is also what readback and a
    //    region copy observe.
    //  - The sampler state is the target's own (`RenderTarget::filter()`/`address()`), fixed at
    //    creation, so the bind allocates nothing and a target is always read with the same state.
    //  - Record it outside a render pass, the same rule `copy_target_region` and `blit_target`
    //    follow: the backend records the layout transition the sampler needs at that point, where
    //    no attachment of an open pass is involved. Sampling the target of the pass being recorded
    //    is therefore not expressible - which is the point, and why the consumer breaks its pass,
    //    copies, binds, and only then opens the pass that reads the copy.
    //  - A pipeline must have been bound (as for `bind_texture`), and it may be bound before
    //    `begin_render_pass`: the usual recording order is `bind_pipeline` -> `bind_sampled_target`
    //    -> `begin_render_pass` -> `set_viewport`/`set_scissor` -> `push_constants` -> `draw`.
    //    `resume_pass()` in the batched renderer rebinds its own pipeline, so a caller that samples
    //    a target and then resumes rebinds the sampling pipeline after the resume.
    //  - The target is left in a shader-read state until something writes it again: a later
    //    `begin_render_pass`, region copy or blit into the same target transitions it back, and a
    //    later read (`copy_target_to_buffer`, another region copy, a blit source, another bind)
    //    works from either state. Backends track it; the RHI exposes no image layout.
    //  - `slot` selects the descriptor set like `bind_texture`'s slot; the bound pipeline's layout
    //    must declare a combined image sampler there.
    //  - Violations throw `std::invalid_argument` (presentable target, target without content) or
    //    `std::logic_error` (inside a render pass), never silently ignored.
    virtual void bind_sampled_target(std::uint32_t slot, const RenderTarget& target) = 0;
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Scissor& scissor) = 0;
    virtual void push_constants(const void* data, std::size_t size) = 0;
    virtual void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) = 0;
    virtual void copy_target_to_buffer(const RenderTarget& target, Buffer& buffer) = 0;

    // Copies the pixel region `source_region` of `source` into `destination` at
    // `destination_offset`, 1:1 (no scaling, no filtering). This is the isolated-layer seam that
    // docs/design/gpu/offscreen-layers.md defines: the batched 2D renderer ends its render pass,
    // copies what it already drew into an offscreen target, and continues (#244 backdrop blur
    // filters the copy; #227 layer isolation paints a whole subtree into one). Both targets must be
    // 1-sample, non-presentable offscreen targets with the same format. `source` must have rendered
    // content and must not have an open render pass; `source` and `destination` must be different
    // targets. Presentable (swapchain) images are not a copy source or destination in RHI 1.1: they
    // are created for color attachment/transfer-dst, so a window that needs the seam through the
    // swapchain is a swapchain-usage decision, not an RHI one. `destination` is left readable in
    // the same state `end_render_pass()` leaves an offscreen target (TRANSFER_SRC on Vulkan), so
    // `copy_target_to_buffer`, another region copy, or a resample can read it afterwards.
    // `source_region` must lie inside the source extent and `destination_offset + source_region`
    // inside the destination extent; invalid input throws std::invalid_argument, never clips
    // silently. Backends record vkCmdCopyImage / wgpuCommandEncoderCopyTextureToTexture with the
    // layout transitions implied by the RHI contract; no caller-visible barrier exists.
    virtual void copy_target_region(const RenderTarget& source, const Scissor& source_region,
                                    const RenderTarget& destination,
                                    Offset2D destination_offset = {}) = 0;

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

    // --- Compute (RHI 1.16, issue #683) ---------------------------------------------------------
    // Compute work is recorded outside a render pass, in the same list and submission as the
    // render passes around it. Every call below throws std::runtime_error on a backend whose
    // `Device::supports_compute()` is false; callers ask the capability first.
    //
    // Binds `pipeline` for the dispatches that follow. The pipeline must outlive the submission.
    virtual void bind_compute_pipeline(const ComputePipeline& /*pipeline*/) {
        throw std::runtime_error("bind_compute_pipeline: this backend has no compute");
    }
    // Binds `buffer` at binding `slot` of the bound compute pipeline's descriptor set. The buffer
    // must be a `BufferUsage::Storage` buffer of this device and non-empty, `slot` below the
    // pipeline's storage_buffer_count; violations throw std::invalid_argument. The set belongs to
    // the pipeline object, so one submission binds a given pipeline's buffers once: two dispatches
    // of the same pipeline with different buffers need two pipeline objects (or one buffer and a
    // push-constant offset).
    virtual void bind_compute_buffer(std::uint32_t /*slot*/, const Buffer& /*buffer*/) {
        throw std::runtime_error("bind_compute_buffer: this backend has no compute");
    }
    virtual void compute_push_constants(const void* /*data*/, std::size_t /*size*/) {
        throw std::runtime_error("compute_push_constants: this backend has no compute");
    }
    // Runs `x * y * z` workgroups. Each count must be non-zero and within the device's limit
    // (65535 on every device this RHI targets); a zero or larger count throws std::invalid_argument.
    virtual void dispatch(std::uint32_t /*x*/, std::uint32_t /*y*/, std::uint32_t /*z*/) {
        throw std::runtime_error("dispatch: this backend has no compute");
    }
    // Makes every storage-buffer write recorded so far visible to the compute dispatches recorded
    // after it. A dispatch that reads what an earlier one wrote needs one between them.
    virtual void compute_barrier() {
        throw std::runtime_error("compute_barrier: this backend has no compute");
    }
    // Copies `size` bytes from `source` to `destination` at offset 0, ordered after the compute
    // work recorded before it. Both buffers belong to this device; the destination is usually a
    // `Readback` buffer (the way a `gpu_read_write` buffer is read on the CPU).
    virtual void copy_buffer(const Buffer& /*source*/, Buffer& /*destination*/,
                             std::size_t /*size*/) {
        throw std::runtime_error("copy_buffer: this backend has no compute");
    }

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

    // --- GPU frame-time signal (optional; docs/design/performance.md section 6.1) --------------
    // Arms this submission to record a GPU timestamp pair into the device's bounded ring at
    // `slot`. Must be called before the first command is recorded. Returns false when the device
    // reports no usable timestamps (Device::timestamp_capability()), so the caller keeps the CPU
    // frame-time signal; the default implementation is that fallback. The caller owns slot reuse:
    // it may reuse `slot` only after the previous submission that used it has completed (poll
    // is_complete()), which is what keeps the ring read non-blocking.
    virtual bool begin_gpu_timestamps(std::uint32_t /*slot*/) {
        return false;
    }

    // The submitted frame's GPU interval in milliseconds. Only valid after is_complete() returned
    // true for this list; it reads an already-completed timestamp query and never waits. False
    // when no timestamps were recorded or the values are unusable (zero/absurd delta), in which
    // case the caller falls back to the CPU sample.
    virtual bool gpu_frame_time_ms(double& /*out_ms*/) const {
        return false;
    }
};

// Device health for the D7 production policy. A loss is recoverable backend state, not an
// exception: operations that can report it (swapchain acquire/present, command `wait()`) return an
// error/false and the caller polls `status()`. After a loss every RHI object created before it is
// invalid and must be destroyed and recreated (the backend generation-guards destructors so a late
// destruction is safe); the device itself can be recreated with `recover()`.
enum class DeviceStatus : std::uint8_t { Ready, Lost };

enum class DeviceRecoveryError : std::uint8_t {
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
//
// `categories` (RHI 1.14, issue #703) splits the same live bytes by *purpose* instead of by kind,
// which is what memory-model-v1 section 4 budgets. The two views are not additive with each other:
// `buffer_bytes + image_bytes` and the sum of `categories` both describe the live set, so a backend
// that fills both must keep them consistent. A backend that cannot attribute a resource leaves it
// in `MemoryCategory::Other` rather than guessing.
struct DeviceMemoryStats {
    std::uint64_t buffer_bytes = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t allocation_bytes = 0;
    std::uint32_t block_count = 0;
    std::uint32_t allocation_count = 0;
    // Indexed by `static_cast<std::size_t>(MemoryCategory)`. All zero on backends that do not
    // track categories.
    std::array<MemoryCategoryStats, kMemoryCategoryCount> categories{};
};

// Static and runtime capabilities a backend reports (ADR-049). `rhi_version` is the RHI contract
// version the backend implements; backends must declare it with a static_assert against
// kRhiVersion and report the same value here for runtime and conformance logging.
// Not named `DeviceCapabilities`: <wingdi.h> defines that as a macro (-> ...W/...A), and this
// public header must stay includable from translation units that also include Windows headers.
struct RhiCapabilities {
    Backend backend = Backend::Null;
    std::uint32_t rhi_version = kRhiVersion;
    GpuTimestampCapability timestamps{};
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Backend backend() const = 0;

    // Capabilities this backend reports (rhi::kRhiVersion implemented, timestamp probe, etc.).
    [[nodiscard]] virtual RhiCapabilities capabilities() const {
        return RhiCapabilities{backend(), kRhiVersion, timestamp_capability()};
    }
    virtual std::unique_ptr<Buffer> create_buffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> create_texture(const TextureDesc& desc) = 0;
    virtual std::unique_ptr<Pipeline> create_pipeline(const PipelineDesc& desc) = 0;
    virtual std::unique_ptr<RenderTarget> create_render_target(const RenderTargetDesc& desc) = 0;
    virtual std::unique_ptr<CommandList> create_command_list() = 0;

    // RHI 1.16 (issue #683): compute pipelines, read/write storage buffers and the dispatch calls on
    // `CommandList`. The only authority on the capability; false means `create_compute_pipeline`
    // throws std::runtime_error and a caller keeps its CPU path.
    [[nodiscard]] virtual bool supports_compute() const {
        return false;
    }
    virtual std::unique_ptr<ComputePipeline> create_compute_pipeline(const ComputePipelineDesc&) {
        throw std::runtime_error("create_compute_pipeline: this backend has no compute "
                                 "(supports_compute() is false)");
    }

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

    // True when this device can create a `BlendMode::DualSource` pipeline (RHI 1.5, issue #471).
    // This is the only authority on the capability: consumers must not infer it from the backend
    // name, because dual-source blending is an optional feature on Vulkan and absent on WebGPU.
    // When it is false, `create_pipeline` with `BlendMode::DualSource` throws instead of mapping
    // the mode to another blend, and the caller takes the documented fallback (grayscale coverage,
    // one alpha channel). Pure virtual on purpose: a backend that does not answer cannot honour the
    // contract, and the answer is a single recorded device fact, not a guess.
    [[nodiscard]] virtual bool supports_dual_source_blend() const = 0;

    // True when `create_texture` can build a mip chain (`TextureDesc::levels > 1`) for `format`
    // (RHI 1.6, issue #491). The only authority: Vulkan needs linear-filtered blit support for the
    // format, WebGPU has no chain generation yet. When false, `create_texture` with levels > 1
    // throws and the caller samples the single level. Pure virtual so every backend answers.
    [[nodiscard]] virtual bool supports_texture_mipmaps(TextureFormat format) const = 0;

    // How many sampled images one `bind_texture_array` call can bind (RHI 1.7, issue #480 part 2).
    // 0 means the backend has no texture array at all and callers keep the one-set-per-texture
    // path; a positive value is the array length the shader's descriptor array was declared with,
    // so it is also the largest index a shader may use. The only authority on the capability:
    // consumers must not infer it from the backend name, because descriptor indexing is optional on
    // Vulkan and absent on WebGPU. The default is 0, which is the documented fallback.
    [[nodiscard]] virtual std::uint32_t texture_array_capacity() const {
        return 0;
    }

    // True when this device can bind a read-only storage buffer
    // (`CommandList::bind_storage_buffer`, RHI 1.12, ADR-068). This is the only authority on the
    // capability: consumers must not infer it from the backend name, because a read-only storage
    // buffer in the fragment stage is optional on Vulkan and absent from some Tier-2 backends.
    // When it is false, `create_pipeline` with `PipelineDesc::storage_buffer` throws and the caller
    // keeps the push-constant path (three rounded rectangles, ADR-068 section 5). Pure virtual on
    // purpose, like `supports_dual_source_blend()`: a backend that does not answer cannot honour
    // the contract, and the answer is a recorded device fact, not a guess. A Tier-2 backend may
    // answer false indefinitely (ADR-068, owner decision 2026-11-22); it is a capability, not a
    // deadline.
    [[nodiscard]] virtual bool supports_storage_buffers() const = 0;

    // The device's GPU-timestamp probe (docs/design/performance.md section 6.1). This is the only
    // authority on whether a GPU frame-time sample exists; consumers must not infer it from the
    // backend name or any other device fact. The default is the CPU fallback every backend without
    // timestamp queries (null, software, WebGPU today) keeps.
    [[nodiscard]] virtual GpuTimestampCapability timestamp_capability() const {
        return {};
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
