// SPDX-License-Identifier: MIT
//
// The GPU-timestamp probe (docs/design/performance.md section 6.1), split out of
// vulkan_instance.cpp so device creation stays one responsibility per file. It answers whether the
// frame-submission queue family can record timestamps, whether that family is a graphics family,
// and what period the device reports; a device that cannot is downgraded to the CPU fallback rather
// than left to a consumer's guess.

#include "vulkan_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::rendering::vulkan {

void VulkanDevice::probe_timestamp_capability(const VkPhysicalDeviceProperties& properties) {
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    if (family_count > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());
    }
    rhi::GpuTimestampCapability capability;
    capability.period_ns = properties.limits.timestampPeriod;
    if (static_cast<std::size_t>(queue_family_) < families.size()) {
        const VkQueueFamilyProperties& family = families[queue_family_];
        capability.queries_supported = family.timestampValidBits > 0;
        capability.queue_supported = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    }
    timestamp_query_pool_ = VK_NULL_HANDLE;
    if (capability.usable()) {
        VkQueryPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        pool_info.queryCount = 2 * kGpuTimestampSlots;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(device_, &pool_info, nullptr, &pool) == VK_SUCCESS) {
            timestamp_query_pool_ = pool;
        } else {
            capability = rhi::GpuTimestampCapability{};
        }
    }
    timestamp_capability_ = capability;
}

} // namespace tessera::rendering::vulkan
