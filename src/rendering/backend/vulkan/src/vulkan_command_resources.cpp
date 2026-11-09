// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

namespace tessera::rendering::vulkan {
namespace {

// Idle resources kept for reuse. Extras are destroyed so a burst of lists does not pin memory.
constexpr std::size_t kMaxFreeCommandResources = 8;

} // namespace

CommandResources VulkanDevice::acquire_command_resources() {
    reap_command_resources();

    if (!free_commands_.empty()) {
        const CommandResources resources = free_commands_.back();
        free_commands_.pop_back();
        check_vk(vkResetCommandPool(device_, resources.pool, 0), "vkResetCommandPool");
        check_vk(vkResetFences(device_, 1, &resources.fence), "vkResetFences(command)");
        return resources;
    }

    CommandResources resources;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    check_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &resources.pool),
             "vkCreateCommandPool");

    try {
        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = resources.pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        check_vk(vkAllocateCommandBuffers(device_, &allocate_info, &resources.buffer),
                 "vkAllocateCommandBuffers");

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check_vk(vkCreateFence(device_, &fence_info, nullptr, &resources.fence),
                 "vkCreateFence(command)");
    } catch (...) {
        // Destroying the pool frees its command buffer too.
        destroy_command_resource(resources);
        throw;
    }
    return resources;
}

void VulkanDevice::release_command_resources(const CommandResources& resources, bool in_flight) {
    if (in_flight) {
        // The GPU may still be executing this buffer; it is recycled by reap_command_resources.
        inflight_commands_.push_back(resources);
        return;
    }
    if (free_commands_.size() >= kMaxFreeCommandResources) {
        destroy_command_resource(resources);
        return;
    }
    free_commands_.push_back(resources);
}

void VulkanDevice::reap_command_resources() {
    // Independent submissions may finish in any order, so every entry is checked.
    for (auto it = inflight_commands_.begin(); it != inflight_commands_.end();) {
        if (vkGetFenceStatus(device_, it->fence) == VK_NOT_READY) {
            ++it;
            continue;
        }
        // Signaled, or the device reported an error and the work will never run: either way
        // the submission no longer uses the buffer.
        const CommandResources finished = *it;
        it = inflight_commands_.erase(it);
        release_command_resources(finished, false);
    }
}

void VulkanDevice::destroy_command_resource(const CommandResources& resources) {
    if (resources.fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, resources.fence, nullptr);
    }
    if (resources.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, resources.pool, nullptr);
    }
}

void VulkanDevice::destroy_command_resources() {
    // Callers wait for the device to go idle first, so in-flight entries are finished too.
    for (const CommandResources& resources : inflight_commands_) {
        destroy_command_resource(resources);
    }
    for (const CommandResources& resources : free_commands_) {
        destroy_command_resource(resources);
    }
    inflight_commands_.clear();
    free_commands_.clear();
}

} // namespace tessera::rendering::vulkan
