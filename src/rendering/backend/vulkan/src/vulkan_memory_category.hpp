// SPDX-License-Identifier: MIT
//
// Two small helpers the Vulkan backend shares across translation units, split out of
// vulkan_internal.hpp because that header is at its size baseline and both are self-contained:
//
// * `record_image_transition` - one image memory barrier, with the aspect/level/layer fields every
//   caller in this backend sets identically.
// * `buffer_category` - the buffer-usage to memory-category mapping (RHI 1.14, issue #703).
//
// Both the create path (vulkan_device.cpp) and the destroy path (vulkan_resources.cpp) call
// `buffer_category`, so a buffer is always released from the category it was charged to. Deriving
// the category twice from the same `BufferUsage` is what keeps those two in step without storing a
// second field.

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <volk.h>

namespace tessera::rendering::vulkan {

inline void record_image_transition(VkCommandBuffer command_buffer, VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkAccessFlags src_access, VkAccessFlags dst_access,
                                    VkPipelineStageFlags src_stage,
                                    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// A buffer's `BufferUsage` is already its purpose, so the category is a rename rather than a guess.
// `Storage` maps to `Storage`, not to `Other`: a clip-record list is a real budget line, and
// folding it into `Other` would hide it.
[[nodiscard]] inline rhi::MemoryCategory buffer_category(rhi::BufferUsage usage) {
    switch (usage) {
    case rhi::BufferUsage::Vertex:
        return rhi::MemoryCategory::Vertex;
    case rhi::BufferUsage::Index:
        return rhi::MemoryCategory::Index;
    case rhi::BufferUsage::Uniform:
        return rhi::MemoryCategory::Uniform;
    case rhi::BufferUsage::Staging:
        return rhi::MemoryCategory::Staging;
    case rhi::BufferUsage::Readback:
        return rhi::MemoryCategory::Readback;
    case rhi::BufferUsage::Storage:
        return rhi::MemoryCategory::Storage;
    }
    return rhi::MemoryCategory::Other;
}

} // namespace tessera::rendering::vulkan
