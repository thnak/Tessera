// SPDX-License-Identifier: MIT

#pragma once

#include "vulkan_internal.hpp"

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
};

// Win32 presentation swapchain. Acquire and present are bounded; recreate retires the previous
// VkSwapchainKHR only after a bounded queue drain, never through vkDeviceWaitIdle.
class VulkanSwapchain final : public rhi::Swapchain {
public:
    static std::expected<std::unique_ptr<VulkanSwapchain>, rhi::SwapchainError>
    create(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
           const rhi::SwapchainDesc& desc, VulkanDevice* owner);

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

    [[nodiscard]] const VulkanSwapchainStats& stats() const {
        return stats_;
    }

private:
    VulkanSwapchain(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                    VkQueue queue);

    // True while the swapchain still belongs to the live device generation (D7). A lost generation
    // reports DeviceLost from acquire/present and skips device-owned teardown.
    [[nodiscard]] bool usable() const;

    std::expected<void, rhi::SwapchainError> create_surface(const rhi::SwapchainDesc& desc);
    std::expected<void, rhi::SwapchainError> create_swapchain(bool recreate, rhi::Extent2D extent);
    std::expected<void, rhi::SwapchainError> recreate_pending();
    void ensure_acquire_fence();
    void destroy_swapchain_resources();
    void wait_for_present_bounded();
    void destroy();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFence acquire_fence_ = VK_NULL_HANDLE;
    VkFence drain_fence_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<std::unique_ptr<VulkanRenderTarget>> image_targets_;
    // One per image: signaled by the render submission, waited on by present.
    std::vector<VkSemaphore> render_finished_;
    std::vector<std::uint8_t> image_acquired_;
    rhi::SwapchainDesc desc_;
    rhi::Extent2D extent_;
    rhi::Extent2D pending_extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    std::uint32_t pending_acquire_index_ = 0;
    bool acquire_pending_ = false;
    bool out_of_date_ = false;
    bool resize_requested_ = false;
    bool present_submitted_ = false;
    bool drain_pending_ = false;
    VulkanSwapchainStats stats_;
};

#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE

} // namespace tessera::rendering::vulkan
