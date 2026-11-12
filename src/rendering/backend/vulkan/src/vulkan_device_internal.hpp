// SPDX-License-Identifier: MIT

#pragma once

// Internal Vulkan device definition, included at the end of vulkan_internal.hpp (which supplies the
// RHI wrapper classes, CommandResources, and InjectedDeviceFault). Do not include it directly.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

namespace tessera::rendering::vulkan {

class VulkanTexture;

class VulkanDevice final : public rhi::Device {
public:
    explicit VulkanDevice(const rhi::DeviceDesc& desc);
    ~VulkanDevice() override;

    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::Vulkan;
    }
    [[nodiscard]] bool supports_sample_count(rhi::TextureFormat format,
                                             std::uint32_t sample_count) const override;
    // The RHI timestamp probe: the selected queue family's timestampValidBits, that family's
    // graphics capability, and the device's timestampPeriod (docs/design/performance.md 6.1).
    [[nodiscard]] rhi::GpuTimestampCapability timestamp_capability() const override {
        return timestamp_capability_;
    }
    // Bounded ring of timestamp pairs, allocated once with the device. A pair is reused only after
    // the submission that recorded it completed (the caller owns that slot contract), so a reset
    // never races GPU work still reading the pair.
    static constexpr std::uint32_t kGpuTimestampSlots = 8;
    [[nodiscard]] VkQueryPool timestamp_query_pool() const {
        return timestamp_query_pool_;
    }
    // Reads a completed timestamp pair; false when it is not ready or its interval is unusable.
    // Never waits: no VK_QUERY_RESULT_WAIT_BIT and no fence.
    bool read_timestamp_pair(std::uint32_t pair, double& out_ms) const;
    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override;
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override;
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override;
    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override;
    std::unique_ptr<rhi::CommandList> create_command_list() override;
    std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc& desc) override;
    void wait_idle() override;
    [[nodiscard]] rhi::DeviceMemoryStats memory_statistics() const override;

    // ---- D7 device-loss state -------------------------------------------------------------
    // Losses detected by submit/wait/acquire/present are recorded here; they never throw across
    // the backend seam. Callers see Lost, stop using the device, destroy their RHI objects and
    // call recover().
    [[nodiscard]] rhi::DeviceStatus status() const override {
        return status_;
    }
    std::expected<void, rhi::DeviceRecoveryError> recover() override;

    [[nodiscard]] std::uint32_t epoch() const {
        return epoch_;
    }
    // True when `epoch` is the live generation and the device is healthy: operations on wrappers
    // from an older generation are refused instead of touching dead handles.
    [[nodiscard]] bool usable_epoch(std::uint32_t epoch) const {
        return status_ == rhi::DeviceStatus::Ready && epoch == epoch_;
    }
    // Destructors may pass handles they own back to the driver. After a real loss the device is
    // gone, so those handles are abandoned (the driver reclaims them with the device); a simulated
    // loss keeps the old device alive, so its handles must still be destroyed to stay validation
    // clean.
    [[nodiscard]] bool destroyable_epoch(std::uint32_t epoch) const {
        return epoch == epoch_ || (status_ == rhi::DeviceStatus::Lost && simulated_loss_);
    }
    // Per-kind live bytes for memory_statistics(). A release from before a device loss is ignored:
    // the counters restart at zero when the generation changes.
    void account_buffer(std::int64_t delta, std::uint32_t epoch) {
        account(buffer_bytes_, delta, epoch);
    }
    void account_image(std::int64_t delta, std::uint32_t epoch) {
        account(image_bytes_, delta, epoch);
    }
    void resource_created() {
        ++live_resources_;
    }
    void resource_destroyed() {
        if (live_resources_ > 0) {
            --live_resources_;
        }
    }
    [[nodiscard]] std::size_t live_resources() const {
        return live_resources_;
    }

    // Records a loss without issuing Vulkan calls (the device may already be gone). The epoch is
    // bumped so every existing RHI object becomes stale. Idempotent per loss.
    void mark_device_lost();

    // Test seam: consumes a pending injected fault of `fault`. Always false without
    // TESSERA_VULKAN_TEST_HOOKS.
    bool consume_injected_fault(InjectedDeviceFault fault);

#if defined(TESSERA_VULKAN_TEST_HOOKS)
    void inject_fault_for_testing(InjectedDeviceFault fault) {
        injected_fault_ = fault;
    }
    // Marks the device lost as if the driver reported VK_ERROR_DEVICE_LOST, while keeping the real
    // device alive so the test can release resources and run recovery (which recreates the device).
    void simulate_device_loss_for_testing();
    // Forces the next recover() to report DeviceCreationFailed without consuming a real attempt.
    void fail_next_recovery_for_testing() {
        fail_recovery_for_testing_ = true;
    }
#endif

    // Full-image upload used by Texture::write (dynamic atlases).
    void upload_texture(VulkanTexture& texture, const void* data, std::size_t size);

    // Hands out recycled command resources (or creates new ones). release_command_resources takes
    // them back; `in_flight` resources are reused only once their fence has signaled.
    CommandResources acquire_command_resources();
    void release_command_resources(const CommandResources& resources, bool in_flight);
    void destroy_command_resource(const CommandResources& resources);

    // Records `record` into its own command buffer and queues it without waiting. `staging`, if
    // any, is kept alive until the GPU has finished with it.
    void submit_upload(const std::function<void(VkCommandBuffer)>& record,
                       std::unique_ptr<rhi::Buffer> staging);

    // Runs `destroy` once every upload submitted so far has finished (immediately if none is
    // pending). Textures use this so an image is never destroyed under an in-flight copy.
    void retire_after_uploads(std::function<void()> destroy);

private:
    void require_ready(const char* what) const;
    void reap_command_resources();
    void destroy_command_resources();
    void create_instance();
    void pick_device();
    void create_device();
    void create_allocator();
    void create_descriptor_layout();
    void create_descriptor_pool();
    void reap_uploads();
    void drain_uploads();
    void destroy_device_objects();
    void reset_lost_bookkeeping();
    void destroy();

    // An upload submitted without waiting. The staging buffer and command resources stay alive
    // until the fence signals; `serial` orders it against deferred destruction.
    struct PendingUpload {
        CommandResources commands;
        std::unique_ptr<rhi::Buffer> staging;
        std::uint64_t serial = 0;
    };
    struct RetiredResource {
        std::function<void()> destroy;
        std::uint64_t serial = 0;
    };
    std::vector<PendingUpload> pending_uploads_;
    std::vector<RetiredResource> retired_;
    std::uint64_t upload_serial_ = 0;

    rhi::DeviceDesc desc_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<CommandResources> free_commands_;
    std::vector<CommandResources> inflight_commands_;
#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    bool surface_extensions_enabled_ = false;
    bool swapchain_extension_enabled_ = false;
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    bool win32_present_supported_ = false;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    bool xlib_present_supported_ = false;
#endif

    rhi::DeviceStatus status_ = rhi::DeviceStatus::Ready;
    std::uint32_t epoch_ = 1;
    std::size_t live_resources_ = 0;
    std::int64_t buffer_bytes_ = 0;
    std::int64_t image_bytes_ = 0;
    rhi::GpuTimestampCapability timestamp_capability_{};
    VkQueryPool timestamp_query_pool_ = VK_NULL_HANDLE;

    void account(std::int64_t& counter, std::int64_t delta, std::uint32_t epoch) {
        if (epoch == epoch_) {
            counter = counter + delta < 0 ? 0 : counter + delta;
        }
    }
    bool recovery_attempted_ = false;
    bool simulated_loss_ = false;
#if defined(TESSERA_VULKAN_TEST_HOOKS)
    InjectedDeviceFault injected_fault_ = InjectedDeviceFault::NoFault;
    bool fail_recovery_for_testing_ = false;
#endif
};

} // namespace tessera::rendering::vulkan
