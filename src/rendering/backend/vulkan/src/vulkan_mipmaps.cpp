// SPDX-License-Identifier: MIT
//
// Mip chain upload for 2D textures (RHI 1.6, issue #491).
//
// The base level comes from the caller; every lower level is a linear blit of the one above, the
// standard downsample every driver supports for formats that report BLIT_SRC/BLIT_DST and linear
// filtering (`VulkanDevice::supports_texture_mipmaps`). It runs on the upload command buffer at
// texture creation, so the texture is complete before its first draw and never changes afterward.

#include "vulkan_internal.hpp"

#include <algorithm>

namespace tessera::rendering::vulkan {
namespace {

// A barrier over `level_count` levels starting at `base_level`; record_image_transition only covers
// level 0, which is all a single-level texture has.
void record_level_barrier(VkCommandBuffer command_buffer, VkImage image, std::uint32_t base_level,
                          std::uint32_t level_count, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = base_level;
    barrier.subresourceRange.levelCount = level_count;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

VkOffset3D level_corner(rhi::Extent2D extent, std::uint32_t level) {
    return {static_cast<std::int32_t>(std::max<std::uint32_t>(1U, extent.width >> level)),
            static_cast<std::int32_t>(std::max<std::uint32_t>(1U, extent.height >> level)), 1};
}

} // namespace

void record_texture_upload_with_mips(VkCommandBuffer command_buffer, VkImage image,
                                     VkBuffer staging, rhi::Extent2D extent,
                                     std::uint32_t row_length_texels, std::uint32_t levels) {
    // Every level starts as a transfer destination: level 0 receives the upload, the others the
    // blits.
    record_level_barrier(command_buffer, image, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.bufferRowLength = row_length_texels;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyBufferToImage(command_buffer, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    for (std::uint32_t level = 1; level < levels; ++level) {
        // The level above is finished: make its writes visible as a blit source.
        record_level_barrier(command_buffer, image, level - 1, 1,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = level_corner(extent, level - 1);
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = level_corner(extent, level);
        vkCmdBlitImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    // Levels [0, levels - 1) were left as blit sources; the last level is still a destination.
    if (levels > 1) {
        record_level_barrier(command_buffer, image, 0, levels - 1,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    record_level_barrier(command_buffer, image, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

} // namespace tessera::rendering::vulkan
