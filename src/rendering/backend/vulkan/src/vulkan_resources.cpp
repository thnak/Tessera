// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

#include <cstring>

namespace tessera::rendering::vulkan {

VulkanBuffer::~VulkanBuffer() {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

void VulkanBuffer::write(const void* data, std::size_t size, std::size_t offset) {
    if (offset + size > size_) {
        return;
    }
    if (mapped_ != nullptr) {
        std::memcpy(static_cast<std::uint8_t*>(mapped_) + offset, data, size);
        return;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS) {
        return;
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, data, size);
    vmaUnmapMemory(allocator_, allocation_);
}

void VulkanBuffer::read(void* data, std::size_t size, std::size_t offset) const {
    if (offset + size > size_) {
        return;
    }
    if (mapped_ != nullptr) {
        std::memcpy(data, static_cast<const std::uint8_t*>(mapped_) + offset, size);
        return;
    }

    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS) {
        return;
    }
    std::memcpy(data, static_cast<const std::uint8_t*>(mapped) + offset, size);
    vmaUnmapMemory(allocator_, allocation_);
}

VulkanTexture::~VulkanTexture() {
    // Return the set so texture churn (atlas growth, resize) cannot exhaust the fixed-size pool.
    if (descriptor_set_ != VK_NULL_HANDLE && descriptor_pool_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, descriptor_pool_, 1, &descriptor_set_);
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
}

VulkanPipeline::~VulkanPipeline() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

VulkanRenderTarget::~VulkanRenderTarget() {
    if (!owns_) {
        return;
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
}

} // namespace tessera::rendering::vulkan
