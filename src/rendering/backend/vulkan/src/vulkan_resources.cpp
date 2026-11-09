// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

#include <cstring>
#include <stdexcept>

namespace tessera::rendering::vulkan {

VulkanBuffer::~VulkanBuffer() {
    if (owner_ != nullptr) {
        if (buffer_ != VK_NULL_HANDLE && owner_->destroyable_epoch(epoch_)) {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }
        owner_->resource_destroyed();
        return;
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

namespace {

// A range outside the buffer is a caller bug; dropping the write silently would corrupt the frame
// without any trace. The comparison avoids overflowing offset + size.
void check_range(std::size_t buffer_size, std::size_t size, std::size_t offset, const char* what) {
    if (size > buffer_size || offset > buffer_size - size) {
        throw std::out_of_range(std::string(what) + ": range exceeds the buffer size");
    }
}

} // namespace

void VulkanBuffer::write(const void* data, std::size_t size, std::size_t offset) {
    check_range(size_, size, offset, "VulkanBuffer::write");
    if (owner_ != nullptr && !owner_->usable_epoch(epoch_)) {
        throw std::runtime_error("VulkanBuffer::write: buffer belongs to a lost device generation");
    }
    if (mapped_ != nullptr) {
        std::memcpy(static_cast<std::uint8_t*>(mapped_) + offset, data, size);
        return;
    }

    void* mapped = nullptr;
    check_vk(vmaMapMemory(allocator_, allocation_, &mapped), "vmaMapMemory(write)");
    std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, data, size);
    vmaUnmapMemory(allocator_, allocation_);
}

void VulkanBuffer::read(void* data, std::size_t size, std::size_t offset) const {
    check_range(size_, size, offset, "VulkanBuffer::read");
    if (owner_ != nullptr && !owner_->usable_epoch(epoch_)) {
        throw std::runtime_error("VulkanBuffer::read: buffer belongs to a lost device generation");
    }
    if (mapped_ != nullptr) {
        std::memcpy(data, static_cast<const std::uint8_t*>(mapped_) + offset, size);
        return;
    }

    void* mapped = nullptr;
    check_vk(vmaMapMemory(allocator_, allocation_, &mapped), "vmaMapMemory(read)");
    std::memcpy(data, static_cast<const std::uint8_t*>(mapped) + offset, size);
    vmaUnmapMemory(allocator_, allocation_);
}

VulkanTexture::~VulkanTexture() {
    const bool destroy_handles = owner_ == nullptr || owner_->destroyable_epoch(epoch_);
    const auto destroy = [device = device_, allocator = allocator_, image = image_, view = view_,
                          sampler = sampler_, allocation = allocation_, pool = descriptor_pool_,
                          set = descriptor_set_] {
        // Return the set so texture churn (atlas growth, resize) cannot exhaust the fixed-size
        // pool.
        if (set != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device, pool, 1, &set);
        }
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, image, allocation);
        }
    };

    if (!destroy_handles) {
        if (owner_ != nullptr) {
            owner_->resource_destroyed();
        }
        return;
    }

    // Uploads are queued without waiting, so one may still be copying into this image. The
    // device destroys the handles once every upload submitted so far has finished.
    if (owner_ != nullptr) {
        owner_->retire_after_uploads(destroy);
        owner_->resource_destroyed();
    } else {
        destroy();
    }
}

VulkanPipeline::~VulkanPipeline() {
    if (owner_ != nullptr) {
        if (owner_->destroyable_epoch(epoch_)) {
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
            }
            if (layout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, layout_, nullptr);
            }
        }
        owner_->resource_destroyed();
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

VulkanRenderTarget::~VulkanRenderTarget() {
    if (!owns_) {
        if (owner_ != nullptr) {
            owner_->resource_destroyed();
        }
        return;
    }
    const bool destroy_handles = owner_ == nullptr || owner_->destroyable_epoch(epoch_);
    if (destroy_handles) {
        if (view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view_, nullptr);
        }
        if (image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, image_, allocation_);
        }
    }
    if (owner_ != nullptr) {
        owner_->resource_destroyed();
    }
}

} // namespace tessera::rendering::vulkan
