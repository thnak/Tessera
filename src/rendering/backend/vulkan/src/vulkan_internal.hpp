// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "vulkan_memory_category.hpp"
#include "vulkan_texture_math.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <volk.h>

// VMA annotates pointers with _Nullable/_Nonnull, a Clang extension that -Werror would reject.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Presentation surfaces exist only on platforms whose WSI extension is compiled in. Everything
// else keeps the offscreen device and reports SwapchainError::NotSupported.
#if defined(VK_USE_PLATFORM_WIN32_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||                     \
    defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_ANDROID_KHR)
#define TESSERA_VULKAN_HAS_PRESENT_SURFACE 1
#endif

namespace tessera::rendering::vulkan {

class VulkanDevice;

// The presentation surface family a device was created for (issue #155): the compiled-in families
// are probed at creation and `desc_.surface` (or, for Auto, the session environment) selects one.
// A build may carry both Xlib and Wayland support; only the selected family is accepted by
// create_swapchain(), so a Wayland device never tries to build an Xlib surface from wl_surface*.
// `NoPlatform` rather than `None` because <X11/Xlib.h> defines `None` as a macro.
enum class PresentPlatform : std::uint8_t { NoPlatform, Win32, Xlib, Wayland, Android };

// Validation message counter plumbing (vulkan_validation.cpp; public accessors in
// tessera/rendering/vulkan/vulkan_validation.hpp). Only the debug callback writes it.
void record_validation_message() noexcept;
void mark_validation_active() noexcept;

// Test seam (compiled in when TESSERA_VULKAN_TEST_HOOKS is defined) that reports a loss from the
// next matching operation instead of waiting for a real GPU reset. The "no fault" enumerator is
// `NoFault`, not `None`: X11's X.h defines `None` as a macro, so `None` would break this header on
// every Linux Xlib build.
enum class InjectedDeviceFault : std::uint8_t {
    NoFault,
    DeviceLostOnSubmit,
    DeviceLostOnFenceWait,
    DeviceLostOnAcquire,
    DeviceLostOnPresent,
};

// RHI objects are only ever created by the matching device, so passing one from another backend is
// a contract violation, not a runtime condition. Debug builds verify the type; release builds do
// a plain static_cast so the frame path pays for no RTTI.
template<typename To, typename From> To& vulkan_cast(From& from) {
    assert(dynamic_cast<To*>(&from) != nullptr && "RHI object belongs to a different backend");
    return static_cast<To&>(from);
}

inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult=" +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

inline VkFormat to_vk_format(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::TextureFormat::R8Unorm:
        return VK_FORMAT_R8_UNORM;
    case rhi::TextureFormat::B8G8R8A8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case rhi::TextureFormat::R8G8B8A8Srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case rhi::TextureFormat::B8G8R8A8Srgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}

inline VkFormat to_vk_vertex_format(rhi::VertexFormat format) {
    switch (format) {
    case rhi::VertexFormat::Float:
        return VK_FORMAT_R32_SFLOAT;
    case rhi::VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case rhi::VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case rhi::VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case rhi::VertexFormat::UByte4Norm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::VertexFormat::UInt:
        return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

inline VkSampleCountFlagBits to_vk_sample_count(std::uint32_t sample_count) {
    switch (sample_count) {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    default:
        break;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

// Rejects unsupported sample counts with an explicit value instead of silently lowering them.
inline std::uint32_t checked_sample_count(std::uint32_t sample_count, const char* what) {
    if (!rhi::is_supported_sample_count(sample_count)) {
        throw std::invalid_argument(std::string(what) + ": unsupported sample count " +
                                    std::to_string(sample_count) + " (expected 1 or 4)");
    }
    return sample_count;
}

inline VkFilter to_vk_filter(rhi::Filter filter) {
    return filter == rhi::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

inline VkSamplerAddressMode to_vk_address(rhi::AddressMode mode) {
    return mode == rhi::AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                            : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

inline VkAttachmentLoadOp to_vk_load_op(rhi::LoadOp op) {
    switch (op) {
    case rhi::LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case rhi::LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case rhi::LoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

inline VkAttachmentStoreOp to_vk_store_op(rhi::StoreOp op) {
    return op == rhi::StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                     : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// The stage/access pair a barrier waits on to make a render target's readable image available in
// its current layout. RHI 1.1 left every offscreen target in TRANSFER_SRC_OPTIMAL; RHI 1.3 adds
// SHADER_READ_ONLY_OPTIMAL (`bind_sampled_target`), so a consumer can no longer assume one state
// and reads the target's tracked layout instead. The mapping lives in vulkan_sampled_target.cpp.
struct LayoutSource {
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags access = 0;
};

[[nodiscard]] LayoutSource source_for_layout(VkImageLayout layout);

class VulkanBuffer final : public rhi::Buffer {
public:
    VulkanBuffer(VulkanDevice* owner, std::uint32_t epoch, VmaAllocator allocator, VkBuffer buffer,
                 VmaAllocation allocation, std::size_t size, void* mapped,
                 rhi::BufferUsage usage = rhi::BufferUsage::Vertex)
        : owner_(owner),
          epoch_(epoch),
          allocator_(allocator),
          buffer_(buffer),
          allocation_(allocation),
          size_(size),
          mapped_(mapped),
          usage_(usage) {}

    ~VulkanBuffer() override;

    [[nodiscard]] std::size_t size() const override {
        return size_;
    }
    void write(const void* data, std::size_t size, std::size_t offset) override;
    void read(void* data, std::size_t size, std::size_t offset) const override;
    [[nodiscard]] VkBuffer handle() const {
        return buffer_;
    }
    // What the buffer was created for. bind_storage_buffer rejects a buffer that was not created
    // with BufferUsage::Storage (RHI 1.12, ADR-068): a vertex buffer bound as a storage buffer
    // would be a descriptor the driver may reject or, worse, silently accept.
    [[nodiscard]] rhi::BufferUsage usage() const {
        return usage_;
    }

private:
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    std::size_t size_ = 0;
    void* mapped_ = nullptr;
    rhi::BufferUsage usage_ = rhi::BufferUsage::Vertex;
};

class VulkanTexture final : public rhi::Texture {
public:
    VulkanTexture(VkDevice device, VmaAllocator allocator, VkImage image, VkImageView view,
                  VkSampler sampler, VmaAllocation allocation, VkDescriptorPool descriptor_pool,
                  VkDescriptorSet descriptor_set, rhi::Extent2D extent, rhi::TextureFormat format,
                  VulkanDevice* owner, std::uint32_t epoch, std::uint32_t sample_count = 1,
                  std::uint32_t levels = 1,
                  rhi::MemoryCategory category = rhi::MemoryCategory::Other)
        : device_(device),
          allocator_(allocator),
          image_(image),
          view_(view),
          sampler_(sampler),
          allocation_(allocation),
          descriptor_pool_(descriptor_pool),
          descriptor_set_(descriptor_set),
          extent_(extent),
          format_(format),
          owner_(owner),
          epoch_(epoch),
          sample_count_(sample_count),
          levels_(levels),
          category_(category) {}

    ~VulkanTexture() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return sample_count_;
    }
    [[nodiscard]] std::uint32_t levels() const override {
        return levels_;
    }
    void write(const void* data, std::size_t size) override;
    void write_region(const rhi::TextureRegion& region, const void* data,
                      std::size_t row_pitch) override;

    [[nodiscard]] VkImage image() const {
        return image_;
    }
    // The sampled view and sampler of this texture, for a descriptor write (RHI 1.7, issue #480
    // part 2: the texture-array set writes one descriptor per bound image).
    [[nodiscard]] VkImageView view() const {
        return view_;
    }
    [[nodiscard]] VkSampler sampler() const {
        return sampler_;
    }
    [[nodiscard]] VkDescriptorSet descriptor_set() const {
        return descriptor_set_;
    }
    [[nodiscard]] std::uint32_t epoch() const {
        return epoch_;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    std::uint32_t sample_count_ = 1;
    std::uint32_t levels_ = 1;
    rhi::MemoryCategory category_ = rhi::MemoryCategory::Other;
};

class VulkanPipeline final : public rhi::Pipeline {
public:
    VulkanPipeline(VulkanDevice* owner, std::uint32_t epoch, VkDevice device, VkPipeline pipeline,
                   VkPipelineLayout layout, std::uint32_t sample_count)
        : owner_(owner),
          epoch_(epoch),
          device_(device),
          pipeline_(pipeline),
          layout_(layout),
          sample_count_(sample_count) {}

    ~VulkanPipeline() override;

    [[nodiscard]] VkPipeline handle() const {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineLayout layout() const {
        return layout_;
    }
    [[nodiscard]] std::uint32_t epoch() const {
        return epoch_;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return sample_count_;
    }

private:
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::uint32_t sample_count_ = 1;
};

// Frame-pacing binding a swapchain publishes on an acquired image target (docs/design/gpu/frames.md
// section 5.1). The submission that renders the image waits on the slot's binary acquire semaphore
// and signals the swapchain's timeline value, so the swapchain can decide slot reuse by a counter
// query instead of a fence wait. Targets that are not swapchain images leave it empty.
struct FrameSyncBinding {
    VkSemaphore acquire = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t value = 0;
};

class VulkanRenderTarget final : public rhi::RenderTarget {
public:
    VulkanRenderTarget(VulkanDevice* owner, std::uint32_t epoch, VkDevice device,
                       VmaAllocator allocator, VkImage image, VkImageView view,
                       VmaAllocation allocation, rhi::Extent2D extent, rhi::TextureFormat format,
                       bool owns = true, bool presentable = false,
                       VkImage resolve_image = VK_NULL_HANDLE,
                       VkImageView resolve_view = VK_NULL_HANDLE,
                       VmaAllocation resolve_allocation = nullptr, std::uint32_t sample_count = 1,
                       rhi::Filter filter = rhi::Filter::Linear,
                       rhi::AddressMode address = rhi::AddressMode::ClampToEdge)
        : owner_(owner),
          epoch_(epoch),
          device_(device),
          allocator_(allocator),
          image_(image),
          view_(view),
          allocation_(allocation),
          resolve_image_(resolve_image),
          resolve_view_(resolve_view),
          resolve_allocation_(resolve_allocation),
          extent_(extent),
          format_(format),
          owns_(owns),
          presentable_(presentable),
          sample_count_(sample_count),
          filter_(filter),
          address_(address) {}

    ~VulkanRenderTarget() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return sample_count_;
    }
    [[nodiscard]] VkImage image() const {
        return image_;
    }
    [[nodiscard]] VkImageView view() const {
        return view_;
    }
    // RHI 1.3 sampled view: the target is read through its 1-sample image, which is the resolve
    // image for a 4x target - the same image readback and copy_target_region observe.
    [[nodiscard]] VkImageView sampled_view() const {
        return resolve_view_ != VK_NULL_HANDLE ? resolve_view_ : view_;
    }
    [[nodiscard]] rhi::Filter filter() const override {
        return filter_;
    }
    [[nodiscard]] rhi::AddressMode address() const override {
        return address_;
    }
    // Resolved 1-sample image for 4x targets; identical to image() for 1-sample targets.
    [[nodiscard]] VkImage readback_image() const {
        return resolve_image_ != VK_NULL_HANDLE ? resolve_image_ : image_;
    }
    [[nodiscard]] VkImage resolve_image() const {
        return resolve_image_;
    }
    [[nodiscard]] VkImageView resolve_view() const {
        return resolve_view_;
    }
    [[nodiscard]] bool presentable() const override {
        return presentable_;
    }

    // Layout of the readable image (image() for a 1-sample target, resolve_image() for 4x). Command
    // recording tracks it - like content_initialized_ - so the next operation transitions from the
    // state the previous one left instead of assuming one (RHI 1.3: a target can be in
    // TRANSFER_SRC after a render pass/copy/blit, or SHADER_READ_ONLY after a sampled bind).
    void mark_image_layout(VkImageLayout layout) const {
        image_layout_ = layout;
    }
    [[nodiscard]] VkImageLayout image_layout() const {
        return image_layout_;
    }

    // The sampler state of a lazily created sampled view: `bind_sampled_target` has the device
    // create the sampler and descriptor set once, at the target's first bind, and they live until
    // the target dies. This is what keeps a steady-state sampled bind allocation-free.
    [[nodiscard]] VkSampler sampled_sampler() const {
        return sampler_;
    }
    [[nodiscard]] VkDescriptorSet sampled_descriptor_set() const {
        return descriptor_set_;
    }
    void set_sampled_descriptor(VkSampler sampler, VkDescriptorSet descriptor_set,
                                VkDescriptorPool pool) const {
        sampler_ = sampler;
        descriptor_set_ = descriptor_set;
        descriptor_pool_ = pool;
    }

    // Swapchain images carry a binary semaphore (owned by the swapchain). A render submission
    // signals it and present waits on it, so present never races the GPU's rendering. The
    // pending flag says a signal is outstanding; it is const-qualified state because command
    // lists hold targets through const pointers.
    void set_render_finished_semaphore(VkSemaphore semaphore) {
        render_finished_ = semaphore;
    }
    [[nodiscard]] VkSemaphore render_finished_semaphore() const {
        return render_finished_;
    }
    void set_render_pending(bool pending) const {
        render_pending_ = pending;
    }
    [[nodiscard]] bool render_pending() const {
        return render_pending_;
    }

    // Timeline-pacing binding of the frame this image was acquired for: the slot's binary acquire
    // semaphore plus the swapchain timeline value the render submission must signal. `pending` is
    // the "not submitted yet" half of the same contract render_pending() has for the present
    // semaphore, so a second render to one image before present cannot signal a value twice.
    void set_frame_sync(const FrameSyncBinding& binding) const {
        frame_sync_ = binding;
        frame_sync_pending_ = binding.timeline != VK_NULL_HANDLE;
    }
    [[nodiscard]] const FrameSyncBinding& frame_sync() const {
        return frame_sync_;
    }
    [[nodiscard]] bool frame_sync_pending() const {
        return frame_sync_pending_;
    }
    void mark_frame_submitted() const {
        frame_sync_pending_ = false;
    }

    // True once this render target has been rendered to at least once since creation. A fresh
    // target image is UNDEFINED, so a preserving (LoadOp::Load) pass must transition from
    // UNDEFINED instead of assuming the layout the target would have after a first render
    // (PRESENT_SRC for swapchain images, TRANSFER_SRC for offscreen readback targets,
    // COLOR_ATTACHMENT_OPTIMAL for multisampled images). Mutable because command lists hold
    // targets as const.
    void mark_content_initialized() const {
        content_initialized_ = true;
    }
    [[nodiscard]] bool content_initialized() const {
        return content_initialized_;
    }

private:
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImage resolve_image_ = VK_NULL_HANDLE;
    VkImageView resolve_view_ = VK_NULL_HANDLE;
    VmaAllocation resolve_allocation_ = nullptr;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    bool owns_ = true;
    bool presentable_ = false;
    std::uint32_t sample_count_ = 1;
    rhi::Filter filter_ = rhi::Filter::Linear;
    rhi::AddressMode address_ = rhi::AddressMode::ClampToEdge;
    mutable VkImageLayout image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    mutable VkSampler sampler_ = VK_NULL_HANDLE;
    mutable VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    mutable VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    mutable bool render_pending_ = false;
    mutable bool content_initialized_ = false;
    mutable FrameSyncBinding frame_sync_;
    mutable bool frame_sync_pending_ = false;
};

// Command pool, buffer and completion fence for one submission. The device recycles these instead
// of creating and destroying them for every frame.
struct CommandResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

class VulkanCommandList final : public rhi::CommandList {
public:
    VulkanCommandList(VulkanDevice& owner, std::uint32_t epoch, VkDevice device, VkQueue queue,
                      CommandResources resources)
        : owner_(owner),
          epoch_(epoch),
          device_(device),
          queue_(queue),
          resources_(resources),
          buffer_(resources.buffer) {}

    ~VulkanCommandList() override;

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override;
    void end_render_pass() override;
    void bind_pipeline(const rhi::Pipeline& pipeline) override;
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override;
    void bind_texture_array(std::uint32_t slot,
                            const std::vector<const rhi::Texture*>& textures) override;
    void bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) override;
    void bind_storage_buffer(std::uint32_t slot, const rhi::Buffer& buffer) override;
    void set_viewport(const rhi::Viewport& viewport) override;
    void set_scissor(const rhi::Scissor& scissor) override;
    void push_constants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) override;
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override;
    void copy_target_region(const rhi::RenderTarget& source, const rhi::Scissor& source_region,
                            const rhi::RenderTarget& destination,
                            rhi::Offset2D destination_offset = {}) override;
    void blit_target(const rhi::RenderTarget& source, const rhi::RenderTarget& destination,
                     rhi::Filter filter) override;
    void submit() override;
    void submit_async() override;
    [[nodiscard]] bool is_complete() const override;
    [[nodiscard]] bool wait(std::uint64_t timeout_ms) override;

    // GPU frame-time signal (docs/design/performance.md section 6.1). See the RHI contract in
    // device.hpp: the pair is recorded in this submission's command buffer and read only after
    // is_complete(). The device ring index is `slot`, and the caller must not reuse a slot whose
    // previous submission is still in flight.
    bool begin_gpu_timestamps(std::uint32_t slot) override;
    [[nodiscard]] bool gpu_frame_time_ms(double& out_ms) const override;

private:
    [[nodiscard]] bool usable() const;
    void require_usable(const char* what) const;
    // Records the pair's reset and begin timestamp at the first recording opportunity (a query
    // reset cannot be recorded inside a render pass, and the begin must precede every command).
    void record_timestamp_begin();

    static constexpr std::uint32_t kNoTimestampPair = 0xFFFFFFFFU;

    VulkanDevice& owner_;
    std::uint32_t epoch_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    CommandResources resources_;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineLayout current_layout_ = VK_NULL_HANDLE;
    std::uint32_t current_pipeline_sample_count_ = 1;
    const VulkanRenderTarget* current_target_ = nullptr;
    // The last target bound by bind_sampled_target(). Recording a render pass on it would sample an
    // image that is simultaneously a color attachment, which is exactly what the call cannot
    // express; begin_render_pass rejects it instead of leaving a descriptor pointing at a
    // transitioned-away layout.
    const VulkanRenderTarget* sampled_target_ = nullptr;
    // True when the submission's presentable target was written by blit_target() rather than a
    // render pass: the acquire semaphore wait must then be scoped to the transfer stage.
    bool present_written_by_transfer_ = false;
    bool recording_ = false;
    bool render_pass_active_ = false;
    bool submitted_ = false;
    std::uint32_t timestamp_pair_ = kNoTimestampPair;
    bool timestamp_begin_pending_ = false;
    bool timestamp_recorded_ = false;
    // Cached completion so a finished list stops querying the fence.
    mutable bool complete_ = false;
};

} // namespace tessera::rendering::vulkan

#include "vulkan_device_internal.hpp"
