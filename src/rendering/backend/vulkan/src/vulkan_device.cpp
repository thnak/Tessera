// SPDX-License-Identifier: MIT

#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "vulkan_internal.hpp"
#include "vulkan_scope_guard.hpp"
#include "vulkan_swapchain.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace tessera::rendering::vulkan {

static_assert(rhi::kRhiVersionMajor == 1, "Vulkan backend implements RHI 1.x");

rhi::RhiCapabilities VulkanDevice::capabilities() const {
    return rhi::RhiCapabilities{rhi::Backend::Vulkan, rhi::kRhiVersion, timestamp_capability()};
}

bool VulkanDevice::supports_sample_count(rhi::TextureFormat format,
                                         std::uint32_t sample_count) const {
    if (!rhi::is_supported_sample_count(sample_count)) {
        return false;
    }
    if (physical_device_ == VK_NULL_HANDLE) {
        return false;
    }
    const VkFormat vk_format = to_vk_format(format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    VkImageFormatProperties properties{};
    const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
        physical_device_, vk_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &properties);
    if (result != VK_SUCCESS) {
        return false;
    }
    const VkSampleCountFlags requested =
        sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    return (properties.sampleCounts & requested) != 0;
}

bool VulkanDevice::read_timestamp_pair(std::uint32_t pair, double& out_ms) const {
    if (device_ == VK_NULL_HANDLE || timestamp_query_pool_ == VK_NULL_HANDLE ||
        !timestamp_capability_.usable() || pair >= kGpuTimestampSlots) {
        return false;
    }
    std::array<std::uint64_t, 2> values{};
    // No VK_QUERY_RESULT_WAIT_BIT: a pair whose submission has not completed reports
    // VK_NOT_READY, and the caller keeps its CPU fallback rather than waiting (ADR-021).
    const VkResult result =
        vkGetQueryPoolResults(device_, timestamp_query_pool_, pair * 2, 2, sizeof(values),
                              values.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false;
    }
    const std::optional<double> interval =
        rhi::timestamp_interval_ms(values[0], values[1], timestamp_capability_.period_ns);
    if (!interval) {
        return false;
    }
    out_ms = *interval;
    return true;
}

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
    require_ready("create_buffer");
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

    resource_created();
    account_buffer(static_cast<std::int64_t>(desc.size), epoch_);
    return std::make_unique<VulkanBuffer>(this, epoch_, allocator_, buffer, allocation, desc.size,
                                          allocated.pMappedData);
}

std::unique_ptr<rhi::RenderTarget>
VulkanDevice::create_render_target(const rhi::RenderTargetDesc& desc) {
    require_ready("create_render_target");
    const std::uint32_t sample_count =
        checked_sample_count(desc.sample_count, "create_render_target");
    const VkFormat format = to_vk_format(desc.format);

    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, format, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
        (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0 ||
        (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
        throw std::runtime_error(
            "render target format does not support color attachment/copy/sample");
    }
    if (sample_count > 1 && !supports_sample_count(desc.format, sample_count)) {
        throw std::runtime_error("create_render_target: " + std::to_string(sample_count) +
                                 "x MSAA is not supported for this format on this device");
    }

    struct ImageAllocation {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
    };
    const auto create_image = [&](VkSampleCountFlagBits samples,
                                  VkImageUsageFlags usage) -> ImageAllocation {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {desc.extent.width, desc.extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = samples;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        ImageAllocation result;
        check_vk(vmaCreateImage(allocator_, &image_info, &allocation_info, &result.image,
                                &result.allocation, nullptr),
                 "vmaCreateImage(render target)");
        return result;
    };

    // The 1-sample image is the render image for 1x and the resolve target for 4x. A 4x target also
    // owns the multisample render image that is discarded after the resolve.
    ImageAllocation render_image;
    ImageAllocation resolve_image;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView resolve_view = VK_NULL_HANDLE;
    // Any step below can throw (VMA out of memory, view creation); release exactly what exists.
    const auto release_all = [&] {
        if (resolve_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, resolve_view, nullptr);
        }
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
        if (resolve_image.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, resolve_image.image, resolve_image.allocation);
        }
        if (render_image.image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, render_image.image, render_image.allocation);
        }
    };

    const auto create_view = [&](VkImage target_image) -> VkImageView {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = target_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VkImageView created = VK_NULL_HANDLE;
        check_vk(vkCreateImageView(device_, &view_info, nullptr, &created), "vkCreateImageView");
        return created;
    };

    try {
        if (sample_count > 1) {
            render_image =
                create_image(to_vk_sample_count(sample_count), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            // The resolve image is what readback, region copies and a sampled bind observe (RHI
            // 1.3), so it needs SAMPLED alongside the color/transfer usage it already had.
            resolve_image =
                create_image(VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                        VK_IMAGE_USAGE_SAMPLED_BIT);
        } else {
            // TRANSFER_SRC: readback/resample source. TRANSFER_DST: destination of
            // CommandList::copy_target_region, the isolated-layer seam (issue #242), which leaves
            // the target readable in TRANSFER_SRC again. SAMPLED: the RHI 1.3 sampled view
            // (CommandList::bind_sampled_target) the glass composite reads a pooled target through.
            render_image = create_image(VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                   VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        view = create_view(render_image.image);
        if (resolve_image.image != VK_NULL_HANDLE) {
            resolve_view = create_view(resolve_image.image);
        }

        // The target owns images/views from here.
        resource_created();
        const std::int64_t target_bytes =
            render_target_bytes(desc.extent, desc.format, sample_count);
        account_image(target_bytes, epoch_);
        try {
            return std::make_unique<VulkanRenderTarget>(
                this, epoch_, device_, allocator_, render_image.image, view,
                render_image.allocation, desc.extent, desc.format, true, false, resolve_image.image,
                resolve_view, resolve_image.allocation, sample_count, desc.filter, desc.address);
        } catch (...) {
            account_image(-target_bytes, epoch_);
            resource_destroyed();
            throw;
        }
    } catch (...) {
        release_all();
        throw;
    }
}

std::unique_ptr<rhi::CommandList> VulkanDevice::create_command_list() {
    require_ready("create_command_list");
    resource_created();
    return std::make_unique<VulkanCommandList>(*this, epoch_, device_, queue_,
                                               acquire_command_resources());
}

std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
VulkanDevice::create_swapchain(const rhi::SwapchainDesc& desc) {
#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    if (status_ == rhi::DeviceStatus::Lost) {
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
    // A Vulkan swapchain needs a VkSurfaceKHR, which needs the platform window handles from the
    // HAL. Win32 uses native_window (HWND); X11 uses native_display (Display*) + native_window;
    // Wayland uses native_display (wl_display*) + native_window (wl_surface*). Which family the
    // pointers mean is fixed at device creation (desc.surface, see resolve_present_platform), not
    // inferred from an environment variable here.
    if (desc.image_count < 2 || desc.extent.width == 0 || desc.extent.height == 0 ||
        desc.native_window == nullptr) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    if ((present_platform_ == PresentPlatform::Xlib ||
         present_platform_ == PresentPlatform::Wayland) &&
        desc.native_display == nullptr) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    if (!surface_extensions_enabled_ || !swapchain_extension_enabled_ || !has_present_support()) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    auto created = VulkanSwapchain::create(instance_, physical_device_, device_, queue_, desc, this,
                                           present_platform_);
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

rhi::DeviceMemoryStats VulkanDevice::memory_statistics() const {
    rhi::DeviceMemoryStats stats;
    if (allocator_ == nullptr) {
        return stats;
    }
    VmaTotalStatistics total{};
    vmaCalculateStatistics(allocator_, &total);
    stats.buffer_bytes = static_cast<std::uint64_t>(buffer_bytes_);
    stats.image_bytes = static_cast<std::uint64_t>(image_bytes_);
    stats.block_bytes = total.total.statistics.blockBytes;
    stats.allocation_bytes = total.total.statistics.allocationBytes;
    stats.block_count = total.total.statistics.blockCount;
    stats.allocation_count = total.total.statistics.allocationCount;
    return stats;
}

void VulkanDevice::wait_idle() {
    if (device_ == VK_NULL_HANDLE || status_ == rhi::DeviceStatus::Lost) {
        return;
    }
    const VkResult result = vkDeviceWaitIdle(device_);
    if (result == VK_ERROR_DEVICE_LOST) {
        mark_device_lost();
        return;
    }
    check_vk(result, "vkDeviceWaitIdle");
}

std::unique_ptr<rhi::Device> create_vulkan_device(const rhi::DeviceDesc& desc) {
    return std::make_unique<VulkanDevice>(desc);
}

} // namespace tessera::rendering::vulkan
