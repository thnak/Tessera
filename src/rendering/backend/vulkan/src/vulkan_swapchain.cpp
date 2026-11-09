// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "vulkan_surface_format.hpp"
#include "vulkan_swapchain.hpp"

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#include <windows.h>
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif

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

SurfaceFormat to_surface_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_UNDEFINED:
        return SurfaceFormat::Undefined;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return SurfaceFormat::R8G8B8A8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return SurfaceFormat::B8G8R8A8Unorm;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return SurfaceFormat::R8G8B8A8Srgb;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return SurfaceFormat::B8G8R8A8Srgb;
    default:
        return SurfaceFormat::Other;
    }
}

VkFormat to_vk_surface_format(SurfaceFormat format) {
    switch (format) {
    case SurfaceFormat::R8G8B8A8Srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case SurfaceFormat::B8G8R8A8Srgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case SurfaceFormat::B8G8R8A8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case SurfaceFormat::R8G8B8A8Unorm:
    case SurfaceFormat::Undefined:
    case SurfaceFormat::Other:
        break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

SurfaceColorSpace to_color_space(VkColorSpaceKHR color_space) {
    return color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ? SurfaceColorSpace::SrgbNonlinear
                                                            : SurfaceColorSpace::Other;
}

// Applies the D2 preference policy to the driver's surface formats (see vulkan_surface_format.hpp).
bool select_surface_format(const std::vector<VkSurfaceFormatKHR>& formats,
                           VkSurfaceFormatKHR& surface_format, rhi::TextureFormat& rhi_format) {
    std::vector<SurfaceFormatCandidate> candidates;
    candidates.reserve(formats.size());
    for (const VkSurfaceFormatKHR& format : formats) {
        candidates.push_back(SurfaceFormatCandidate{to_surface_format(format.format),
                                                    to_color_space(format.colorSpace)});
    }

    const auto chosen = choose_surface_format(candidates);
    if (!chosen) {
        return false;
    }
    surface_format = formats[*chosen];
    const SurfaceFormatCandidate& candidate = candidates[*chosen];
    if (candidate.format == SurfaceFormat::Undefined) {
        // The driver exposes a single "any format" entry. D2 chooses sRGB for an sRGB color space
        // and the historical UNORM format otherwise.
        const bool srgb = candidate.color_space == SurfaceColorSpace::SrgbNonlinear;
        const SurfaceFormat fallback =
            srgb ? SurfaceFormat::R8G8B8A8Srgb : SurfaceFormat::R8G8B8A8Unorm;
        surface_format.format = to_vk_surface_format(fallback);
        rhi_format = to_rhi_format(fallback);
        return true;
    }
    rhi_format = to_rhi_format(candidate.format);
    return true;
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

bool VulkanSwapchain::usable() const {
    return owner_ == nullptr || owner_->usable_epoch(epoch_);
}

std::expected<std::unique_ptr<VulkanSwapchain>, rhi::SwapchainError>
VulkanSwapchain::create(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                        VkQueue queue, const rhi::SwapchainDesc& desc, VulkanDevice* owner) {
    auto swapchain = std::unique_ptr<VulkanSwapchain>(
        new VulkanSwapchain(instance, physical_device, device, queue));
    swapchain->desc_ = desc;
    swapchain->owner_ = owner;
    swapchain->epoch_ = owner != nullptr ? owner->epoch() : 0;

    if (const auto surface = swapchain->create_surface(desc); !surface) {
        return std::unexpected(surface.error());
    }
    if (const auto created = swapchain->create_swapchain(false, desc.extent); !created) {
        return std::unexpected(created.error());
    }
    return swapchain;
}

std::expected<void, rhi::SwapchainError>
VulkanSwapchain::create_surface(const rhi::SwapchainDesc& desc) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = static_cast<HWND>(desc.native_window);
    const VkResult result = vkCreateWin32SurfaceKHR(instance_, &create_info, nullptr, &surface_);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    if (desc.native_display == nullptr || desc.native_window == nullptr ||
        vkCreateXlibSurfaceKHR == nullptr) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    VkXlibSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    create_info.dpy = static_cast<Display*>(desc.native_display);
    create_info.window =
        static_cast<::Window>(reinterpret_cast<std::uintptr_t>(desc.native_window));
    const VkResult result = vkCreateXlibSurfaceKHR(instance_, &create_info, nullptr, &surface_);
#endif
    if (result != VK_SUCCESS) {
        surface_ = VK_NULL_HANDLE;
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    return {};
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::create_swapchain(bool recreate,
                                                                           rhi::Extent2D extent) {
    if (!usable()) {
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
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
    if (!select_surface_format(formats, surface_format, rhi_format)) {
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

    // The new swapchain has retired the old one; drain the last present before destroying views
    // and the handle, otherwise the presentation engine may still be reading them.
    if (swapchain_ != VK_NULL_HANDLE) {
        wait_for_present_bounded();
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
    image_targets_.clear();
    image_targets_.reserve(images_.size());
    render_finished_.reserve(images_.size());
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
            destroy_swapchain_resources();
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        render_finished_.push_back(semaphore);

        if (owner_ != nullptr) {
            owner_->resource_created();
        }
        image_targets_.push_back(std::make_unique<VulkanRenderTarget>(
            owner_, epoch_, device_, nullptr, images_[i], views_[i], nullptr, extent_, format_,
            false, true));
        image_targets_.back()->set_render_finished_semaphore(semaphore);
    }
    pending_extent_ = chosen_extent;
    resize_requested_ = false;
    out_of_date_ = false;
    ++stats_.swapchains_created;
    return {};
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::recreate_pending() {
    if (!usable()) {
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
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
    image_targets_.clear();
    // Callers drain the queue first, so no submission still signals or waits on these.
    for (const VkSemaphore semaphore : render_finished_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_.clear();
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

#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE
