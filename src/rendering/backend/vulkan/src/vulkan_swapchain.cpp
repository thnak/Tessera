// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "vulkan_internal.hpp"

#if defined(VK_USE_PLATFORM_WIN32_KHR)

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include <windows.h>

namespace tessera::rendering::vulkan {
namespace {

rhi::Extent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities, rhi::Extent2D requested) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return {capabilities.currentExtent.width, capabilities.currentExtent.height};
    }
    rhi::Extent2D extent = requested;
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

bool choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats,
                           VkSurfaceFormatKHR& surface_format, rhi::TextureFormat& rhi_format) {
    constexpr std::array<VkFormat, 2> kPreferred{VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_FORMAT_B8G8R8A8_UNORM};
    for (const VkFormat format : kPreferred) {
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == format &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format = candidate;
                rhi_format = format == VK_FORMAT_R8G8B8A8_UNORM ? rhi::TextureFormat::R8G8B8A8Unorm
                                                                : rhi::TextureFormat::B8G8R8A8Unorm;
                return true;
            }
        }
    }

    // Fall back to a preferred format in a non-SRGB color space rather than failing the surface.
    for (const VkFormat format : kPreferred) {
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == format) {
                surface_format = candidate;
                rhi_format = format == VK_FORMAT_R8G8B8A8_UNORM ? rhi::TextureFormat::R8G8B8A8Unorm
                                                                : rhi::TextureFormat::B8G8R8A8Unorm;
                return true;
            }
        }
    }

    // Some drivers expose a single VK_FORMAT_UNDEFINED entry meaning "any format works".
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        surface_format.format = VK_FORMAT_R8G8B8A8_UNORM;
        surface_format.colorSpace = formats[0].colorSpace;
        rhi_format = rhi::TextureFormat::R8G8B8A8Unorm;
        return true;
    }
    return false;
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    const auto available = [&modes](VkPresentModeKHR mode) {
        return std::find(modes.begin(), modes.end(), mode) != modes.end();
    };
    if (available(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (available(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    // The platform guarantees at least one supported composite mode.
    return static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha &
                                                    ~(capabilities.supportedCompositeAlpha - 1));
}

VkImageViewCreateInfo image_view_info(VkImage image, VkFormat format) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    return view_info;
}

} // namespace

VulkanSwapchain::VulkanSwapchain(VkInstance instance, VkPhysicalDevice physical_device,
                                 VkDevice device, VkQueue queue)
    : instance_(instance),
      physical_device_(physical_device),
      device_(device),
      queue_(queue) {}

VulkanSwapchain::~VulkanSwapchain() {
    destroy();
}

std::expected<std::unique_ptr<VulkanSwapchain>, rhi::SwapchainError>
VulkanSwapchain::create(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                        VkQueue queue, const rhi::SwapchainDesc& desc) {
    auto swapchain = std::unique_ptr<VulkanSwapchain>(
        new VulkanSwapchain(instance, physical_device, device, queue));
    swapchain->desc_ = desc;

    if (const auto surface = swapchain->create_surface(desc.native_window); !surface) {
        return std::unexpected(surface.error());
    }
    if (const auto created = swapchain->create_swapchain(false, desc.extent); !created) {
        return std::unexpected(created.error());
    }
    return swapchain;
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::create_surface(void* native_window) {
    VkWin32SurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = static_cast<HWND>(native_window);
    const VkResult result = vkCreateWin32SurfaceKHR(instance_, &create_info, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        surface_ = VK_NULL_HANDLE;
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    return {};
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::create_swapchain(bool recreate,
                                                                           rhi::Extent2D extent) {
    if (recreate) {
        wait_for_present_bounded();
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities) !=
        VK_SUCCESS) {
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    const rhi::Extent2D chosen_extent = choose_extent(capabilities, extent);
    if (chosen_extent.width == 0 || chosen_extent.height == 0) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    std::uint32_t format_count = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr) !=
            VK_SUCCESS ||
        format_count == 0) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count,
                                             formats.data()) != VK_SUCCESS) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    VkSurfaceFormatKHR surface_format{};
    rhi::TextureFormat rhi_format = rhi::TextureFormat::R8G8B8A8Unorm;
    if (!choose_surface_format(formats, surface_format, rhi_format)) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    std::uint32_t mode_count = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count,
                                                  nullptr) != VK_SUCCESS ||
        mode_count == 0) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    std::vector<VkPresentModeKHR> present_modes(mode_count);
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count,
                                                  present_modes.data()) != VK_SUCCESS) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    const VkPresentModeKHR present_mode = choose_present_mode(present_modes, desc_.vsync);

    const std::uint32_t min_images = std::max(capabilities.minImageCount, 2U);
    const std::uint32_t requested_images = std::max(desc_.image_count, 2U);
    const std::uint32_t max_images = capabilities.maxImageCount == 0
                                         ? requested_images
                                         : std::max(capabilities.maxImageCount, min_images);
    const std::uint32_t image_count = std::clamp(requested_images, min_images, max_images);

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = {chosen_extent.width, chosen_extent.height};
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = choose_composite_alpha(capabilities);
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = swapchain_;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    const VkResult result = vkCreateSwapchainKHR(device_, &create_info, nullptr, &new_swapchain);
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    if (result != VK_SUCCESS) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    // The new swapchain has retired the old one; release its views and handle now.
    if (swapchain_ != VK_NULL_HANDLE) {
        destroy_swapchain_resources();
        ++stats_.swapchain_retires;
    }
    swapchain_ = new_swapchain;

    std::uint32_t actual_images = 0;
    if (vkGetSwapchainImagesKHR(device_, swapchain_, &actual_images, nullptr) != VK_SUCCESS ||
        actual_images == 0) {
        destroy_swapchain_resources();
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    images_.resize(actual_images);
    if (vkGetSwapchainImagesKHR(device_, swapchain_, &actual_images, images_.data()) !=
        VK_SUCCESS) {
        destroy_swapchain_resources();
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    views_.reserve(images_.size());
    for (const VkImage image : images_) {
        VkImageViewCreateInfo view_info = image_view_info(image, surface_format.format);
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &view_info, nullptr, &view) != VK_SUCCESS) {
            destroy_swapchain_resources();
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        views_.push_back(view);
    }

    if (acquire_pending_) {
        // An acquire from the retired swapchain may still be in flight. A fence that is already
        // signaled can be recycled; otherwise abandon the handle rather than reset it in use.
        if (acquire_fence_ != VK_NULL_HANDLE &&
            vkGetFenceStatus(device_, acquire_fence_) == VK_SUCCESS) {
            (void)vkResetFences(device_, 1, &acquire_fence_);
        } else {
            acquire_fence_ = VK_NULL_HANDLE;
        }
        acquire_pending_ = false;
    }
    ensure_acquire_fence();

    image_acquired_.assign(images_.size(), 0);
    format_ = rhi_format;
    extent_ = chosen_extent;
    pending_extent_ = chosen_extent;
    resize_requested_ = false;
    out_of_date_ = false;
    ++stats_.swapchains_created;
    return {};
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::recreate_pending() {
    const rhi::Extent2D requested = resize_requested_ ? pending_extent_ : rhi::Extent2D{0, 0};
    const auto created = create_swapchain(true, requested);
    if (!created) {
        return created;
    }
    resize_requested_ = false;
    return {};
}

void VulkanSwapchain::ensure_acquire_fence() {
    if (acquire_fence_ != VK_NULL_HANDLE) {
        return;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fence_info, nullptr, &acquire_fence_) != VK_SUCCESS) {
        acquire_fence_ = VK_NULL_HANDLE;
    }
}

void VulkanSwapchain::destroy_swapchain_resources() {
    for (const VkImageView view : views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    views_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    images_.clear();
    image_acquired_.clear();
}

} // namespace tessera::rendering::vulkan

#endif // VK_USE_PLATFORM_WIN32_KHR
