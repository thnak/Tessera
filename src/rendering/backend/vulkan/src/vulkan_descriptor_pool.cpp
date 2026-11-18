// SPDX-License-Identifier: MIT
//
// The device's descriptor pool list (issue #480). Every texture and sampled target takes one
// COMBINED_IMAGE_SAMPLER set. A single fixed pool made the number of live textures a hard limit
// that surfaced as a bare vkAllocateDescriptorSets failure; the list grows instead and reports a
// named error at its own cap.

#include "vulkan_internal.hpp"

#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan {

VkDescriptorPool VulkanDevice::add_descriptor_pool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kDescriptorSetsPerPool;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = kDescriptorSetsPerPool;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");
    descriptor_pools_.push_back(pool);
    descriptor_pool_ = pool;
    return pool;
}

// Pools live until the device is torn down: a texture's deferred destruction frees its set into the
// pool it recorded, possibly after the texture's owner has moved on, so a pool is never destroyed
// while an object might still name it.
void VulkanDevice::destroy_descriptor_pools() {
    if (device_ != VK_NULL_HANDLE) {
        for (const VkDescriptorPool pool : descriptor_pools_) {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }
    }
    descriptor_pools_.clear();
    descriptor_pool_ = VK_NULL_HANDLE;
}

VkDescriptorSet VulkanDevice::allocate_descriptor_set(const char* what, VkDescriptorPool& pool) {
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_set_layout_;

    const auto try_pool = [&](VkDescriptorPool candidate, VkDescriptorSet& set) {
        allocate.descriptorPool = candidate;
        const VkResult result = vkAllocateDescriptorSets(device_, &allocate, &set);
        if (result == VK_SUCCESS) {
            pool = candidate;
            return true;
        }
        // A full or fragmented pool is the one answer that means "try elsewhere"; anything else
        // (device loss, out of memory) is a real failure.
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            check_vk(result, what);
        }
        return false;
    };

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (descriptor_pool_ != VK_NULL_HANDLE && try_pool(descriptor_pool_, set)) {
        return set;
    }
    // Freed sets return their space to the pool they came from, so older pools may have room.
    for (const VkDescriptorPool candidate : descriptor_pools_) {
        if (candidate != descriptor_pool_ && try_pool(candidate, set)) {
            descriptor_pool_ = candidate;
            return set;
        }
    }
    if (descriptor_pools_.size() >= kMaxDescriptorPools) {
        throw std::runtime_error(
            std::string(what) + ": descriptor pool limit reached (" +
            std::to_string(descriptor_pools_.size()) + " pools of " +
            std::to_string(kDescriptorSetsPerPool) +
            " sets are all in use); release textures or render targets that are no longer needed");
    }
    if (!try_pool(add_descriptor_pool(), set)) {
        check_vk(VK_ERROR_OUT_OF_POOL_MEMORY, what);
    }
    return set;
}

} // namespace tessera::rendering::vulkan
