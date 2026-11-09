// SPDX-License-Identifier: MIT

#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "vulkan_internal.hpp"
#include "vulkan_swapchain.hpp"

#include <utility>

namespace tessera::rendering::vulkan {

VulkanDevice::VulkanDevice(const rhi::DeviceDesc& desc)
    : desc_(desc) {
    try {
        create_instance();
        pick_device();
        create_device();
        create_allocator();
        create_descriptor_layout();
        create_descriptor_pool();
    } catch (...) {
        destroy();
        throw;
    }
}

VulkanDevice::~VulkanDevice() {
    destroy();
}

std::unique_ptr<rhi::Buffer> VulkanDevice::create_buffer(const rhi::BufferDesc& desc) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = desc.size;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    switch (desc.usage) {
    case rhi::BufferUsage::Vertex:
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        allocation_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case rhi::BufferUsage::Index:
        buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        allocation_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case rhi::BufferUsage::Uniform:
        buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        allocation_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case rhi::BufferUsage::Staging:
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        allocation_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case rhi::BufferUsage::Readback:
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        allocation_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocated{};
    check_vk(vmaCreateBuffer(allocator_, &buffer_info, &allocation_info, &buffer, &allocation,
                             &allocated),
             "vmaCreateBuffer");

    return std::make_unique<VulkanBuffer>(allocator_, buffer, allocation, desc.size,
                                          allocated.pMappedData);
}

std::unique_ptr<rhi::RenderTarget>
VulkanDevice::create_render_target(const rhi::RenderTargetDesc& desc) {
    const VkFormat format = to_vk_format(desc.format);

    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, format, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
        (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
        throw std::runtime_error("render target format does not support color attachment/copy");
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {desc.extent.width, desc.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    check_vk(
        vmaCreateImage(allocator_, &image_info, &allocation_info, &image, &allocation, nullptr),
        "vmaCreateImage(render target)");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    check_vk(vkCreateImageView(device_, &view_info, nullptr, &view), "vkCreateImageView");

    return std::make_unique<VulkanRenderTarget>(device_, allocator_, image, view, allocation,
                                                desc.extent, desc.format);
}

std::unique_ptr<rhi::CommandList> VulkanDevice::create_command_list() {
    return std::make_unique<VulkanCommandList>(*this, device_, queue_, acquire_command_resources());
}

std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
VulkanDevice::create_swapchain(const rhi::SwapchainDesc& desc) {
#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    // A Vulkan swapchain needs a VkSurfaceKHR, which needs the platform window handles from the
    // HAL. Win32 uses native_window (HWND); X11 uses native_display (Display*) + native_window.
    if (desc.image_count < 2 || desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.native_window == nullptr) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    if (desc.native_display == nullptr) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
#endif
    bool present_supported = false;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    present_supported = win32_present_supported_;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    present_supported = xlib_present_supported_;
#endif
    if (!surface_extensions_enabled_ || !swapchain_extension_enabled_ || !present_supported) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    auto created = VulkanSwapchain::create(instance_, physical_device_, device_, queue_, desc);
    if (!created) {
        return std::unexpected(created.error());
    }
    return std::unique_ptr<rhi::Swapchain>(std::move(*created));
#else
    (void)desc;
    // No platform presentation surface; offscreen devices stay usable.
    return std::unexpected(rhi::SwapchainError::NotSupported);
#endif
}

void VulkanDevice::wait_idle() {
    if (device_ != VK_NULL_HANDLE) {
        check_vk(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    }
}

std::unique_ptr<rhi::Device> create_vulkan_device(const rhi::DeviceDesc& desc) {
    return std::make_unique<VulkanDevice>(desc);
}

} // namespace tessera::rendering::vulkan
