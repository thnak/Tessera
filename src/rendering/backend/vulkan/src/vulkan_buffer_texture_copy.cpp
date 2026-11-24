// SPDX-License-Identifier: MIT
//
// `CommandList::copy_buffer_to_texture` (RHI 1.17, ADR-072): the handoff from a compute pass's
// storage buffer (or a staging buffer) to a texture a later draw samples, recorded into the list
// itself so there is no CPU round trip and no extra submission.

#include "vulkan_internal.hpp"
#include "vulkan_memory_category.hpp"
#include "vulkan_texture_math.hpp"

#include <stdexcept>

namespace tessera::rendering::vulkan {

void VulkanCommandList::copy_buffer_to_texture(const rhi::Buffer& source, rhi::Texture& destination,
                                               const rhi::BufferTextureCopy& copy) {
    // Validation first, in the order the null backend and the tests expect: a list in the wrong
    // state is a caller bug whatever the arguments are.
    require_usable("copy_buffer_to_texture");
    if (render_pass_active_) {
        throw std::logic_error("copy_buffer_to_texture: recorded outside a render pass");
    }
    const auto& from = vulkan_cast<const VulkanBuffer>(source);
    auto& to = vulkan_cast<VulkanTexture>(destination);
    if (to.epoch() != epoch_) {
        throw std::runtime_error(
            "copy_buffer_to_texture: texture belongs to a lost device generation");
    }
    if (from.usage() != rhi::BufferUsage::Staging && from.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "copy_buffer_to_texture: the source must be a Staging or Storage buffer");
    }
    if (to.sample_count() != 1 || to.levels() != 1) {
        throw std::invalid_argument(
            "copy_buffer_to_texture: the destination must have one sample and one level");
    }
    const std::size_t pitch =
        rhi::checked_buffer_texture_copy(to.extent(), to.format(), copy, from.size());
    if (pitch == 0) {
        return; // an empty region copies nothing
    }
    ensure_recording("copy_buffer_to_texture");

    // The compute writes to the source must be visible to the transfer read.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);

    // Textures rest in SHADER_READ_ONLY_OPTIMAL (the same round trip as upload_texture_region).
    record_image_transition(buffer_, to.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);

    const rhi::TextureRegion& region = copy.region;
    VkBufferImageCopy image_copy{};
    image_copy.bufferOffset = copy.buffer_offset;
    // The pitch is in texels, and a multiple of 256 bytes is always a whole number of them.
    image_copy.bufferRowLength = static_cast<std::uint32_t>(pitch / bytes_per_pixel(to.format()));
    image_copy.bufferImageHeight = 0;
    image_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_copy.imageSubresource.layerCount = 1;
    image_copy.imageOffset = {static_cast<std::int32_t>(region.x),
                              static_cast<std::int32_t>(region.y), 0};
    image_copy.imageExtent = {region.width, region.height, 1};
    vkCmdCopyBufferToImage(buffer_, from.handle(), to.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &image_copy);

    record_image_transition(buffer_, to.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

} // namespace tessera::rendering::vulkan
