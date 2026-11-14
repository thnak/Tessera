// SPDX-License-Identifier: MIT

#pragma once

#include "vulkan_internal.hpp"

#include <array>

namespace tessera::rendering::vulkan {

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)

struct VulkanSwapchainStats {
    std::uint32_t swapchains_created = 0;
    std::uint32_t swapchain_retires = 0;
    std::uint32_t acquires = 0;
    std::uint32_t presents = 0;
    std::uint32_t out_of_date = 0;
    std::uint32_t suboptimal = 0;
    std::uint32_t timeouts = 0;
    std::uint32_t invalid_presents = 0;
    std::uint32_t resizes = 0;
    // Acquires that found no reusable frame slot and reported Timeout instead of waiting for the
    // GPU (docs/design/gpu/frames.md section 5.1).
    std::uint32_t slots_skipped = 0;
};

// Win32/X11/Wayland presentation swapchain. Acquire and present are bounded; recreate retires the
// previous VkSwapchainKHR only after a bounded queue drain, never through vkDeviceWaitIdle.
// `platform` is the family the owning device resolved at creation (issue #155): it selects the
// VkSurfaceKHR construction and rejects a native handle pair from another family.
//
// Frame pacing uses one timeline semaphore per swapchain generation: an acquire hands the next
// value to the frame, the render submission signals it, and a slot is reusable once
// vkGetSemaphoreCounterValue reports the value reached - a host query, never a wait. Binary
// acquire semaphores stay per slot because image acquisition still needs them; a submission that
// signals the timeline also waits on its slot's acquire semaphore.
class VulkanSwapchain final : public rhi::Swapchain {
public:
    // Two slots match the app driver's two frames in flight; a third would only add latency.
    static constexpr std::uint32_t kFrameSlots = 2;
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFU;

    static std::expected<std::unique_ptr<VulkanSwapchain>, rhi::SwapchainError>
    create(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
           const rhi::SwapchainDesc& desc, VulkanDevice* owner, PresentPlatform platform);

    ~VulkanSwapchain() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    [[nodiscard]] std::uint32_t image_count() const override {
        return static_cast<std::uint32_t>(images_.size());
    }

    [[nodiscard]] std::expected<std::uint32_t, rhi::SwapchainError>
    acquire_next_image(std::uint64_t timeout_ms) override;
    [[nodiscard]] std::expected<void, rhi::SwapchainError> present(std::uint32_t image) override;
    void resize(rhi::Extent2D extent) override;
    [[nodiscard]] rhi::RenderTarget* image_target(std::uint32_t index) override;
    [[nodiscard]] rhi::SwapchainFrameSync frame_sync() const override;
    // The composite mode this generation was created with (glass-v1.md section 6): premultiplied
    // when a per-pixel-alpha request met a surface that offers it, opaque otherwise.
    [[nodiscard]] rhi::SwapchainAlphaMode alpha_mode() const override {
        return alpha_mode_;
    }
    [[nodiscard]] bool supports_per_pixel_alpha() const override {
        return per_pixel_alpha_supported_;
    }
    // True when the surface/format this swapchain was created with supports a filtered blit into
    // its images (a transfer-destination usage plus BLIT_SRC/BLIT_DST on the format). Computed once
    // when the swapchain is created and kept across a recreation.
    [[nodiscard]] bool supports_scaled_present() const override {
        return scaled_present_supported_;
    }

    [[nodiscard]] const VulkanSwapchainStats& stats() const {
        return stats_;
    }

private:
    VulkanSwapchain(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                    VkQueue queue, PresentPlatform platform);

    // Per-frame-slot pacing state. `acquire` is the binary semaphore image acquisition signals and
    // the submission waits on; `value` is the timeline value that submission signals.
    struct FrameSlot {
        VkSemaphore acquire = VK_NULL_HANDLE;
        std::uint64_t value = 0;
        bool in_flight = false;
    };

    // True while the swapchain still belongs to the live device generation (D7). A lost generation
    // reports DeviceLost from acquire/present and skips device-owned teardown.
    [[nodiscard]] bool usable() const;

    std::expected<void, rhi::SwapchainError> create_surface(const rhi::SwapchainDesc& desc);
    std::expected<void, rhi::SwapchainError> create_swapchain(bool recreate, rhi::Extent2D extent);
    std::expected<void, rhi::SwapchainError> recreate_pending();
    // Creates the generation's timeline semaphore and per-slot acquire semaphores (idempotent: a
    // swapchain recreation keeps the values, which stay monotonic for the device generation).
    std::expected<void, rhi::SwapchainError> create_frame_sync();
    void destroy_frame_sync();
    // Releases every slot whose submitted value the GPU reached. No wait: a counter query only.
    void refresh_slots();
    // First slot whose previous submission completed, or kNoSlot when both are still in flight.
    [[nodiscard]] std::uint32_t reusable_slot();
    void destroy_swapchain_resources();
    // Bounded drain for teardown/retirement only (never per frame): the timeline wait covers the
    // last render submission and the empty-submit fence still orders after a queued present.
    void wait_for_present_bounded();
    void destroy();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    PresentPlatform platform_ = PresentPlatform::NoPlatform;
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFence drain_fence_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<std::unique_ptr<VulkanRenderTarget>> image_targets_;
    // One per image: signaled by the render submission, waited on by present.
    std::vector<VkSemaphore> render_finished_;
    std::vector<std::uint8_t> image_acquired_;
    std::array<FrameSlot, kFrameSlots> slots_{};
    // One timeline semaphore owns frame pacing for this device generation.
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::uint64_t timeline_next_ = 1;
    mutable std::uint64_t timeline_completed_ = 0;
    rhi::SwapchainDesc desc_;
    rhi::Extent2D extent_;
    rhi::Extent2D pending_extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    bool out_of_date_ = false;
    bool resize_requested_ = false;
    bool present_submitted_ = false;
    bool drain_pending_ = false;
    bool scaled_present_supported_ = false;
    // Resolved at every swapchain creation: whether the surface offers per-pixel alpha at all, and
    // the mode the current generation actually uses (glass-v1.md section 6).
    bool per_pixel_alpha_supported_ = false;
    rhi::SwapchainAlphaMode alpha_mode_ = rhi::SwapchainAlphaMode::Opaque;
    VulkanSwapchainStats stats_;
};

#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE

} // namespace tessera::rendering::vulkan
