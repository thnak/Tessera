// SPDX-License-Identifier: MIT
//
// CommandList::copy_target_region on Vulkan: render-target region -> render-target region with
// vkCmdCopyImage, including the image-layout transitions the RHI contract hides. This is the
// isolated-layer seam of docs/design/gpu/offscreen-layers.md (issue #242).
//
// Layout bookkeeping: end_render_pass() leaves an offscreen target in TRANSFER_SRC_OPTIMAL, with
// the color write made available to a transfer read. That is exactly the state this operation
// assumes for its source and produces for its destination, so a copy can be chained (copy -> copy
// -> read back) and a later begin_render_pass(LoadOp::Load) transitions from TRANSFER_SRC as usual.
// RHI 1.3 adds a second readable state, SHADER_READ_ONLY_OPTIMAL, for a target bound as a sampled
// texture (vulkan_sampled_target.cpp): a copy that finds its source or destination there
// transitions it back to the transfer state instead of assuming one.

#include "vulkan_internal.hpp"

#include <cstdint>
#include <stdexcept>

namespace tessera::rendering::vulkan {
namespace {

bool region_fits(const rhi::Scissor& region, rhi::Extent2D extent) {
    if (region.x < 0 || region.y < 0 || region.width == 0 || region.height == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(region.x) + region.width <= extent.width &&
           static_cast<std::uint64_t>(region.y) + region.height <= extent.height;
}

bool offset_region_fits(rhi::Offset2D offset, const rhi::Scissor& region, rhi::Extent2D extent) {
    if (offset.x < 0 || offset.y < 0) {
        return false;
    }
    return static_cast<std::uint64_t>(offset.x) + region.width <= extent.width &&
           static_cast<std::uint64_t>(offset.y) + region.height <= extent.height;
}

} // namespace

void VulkanCommandList::copy_target_region(const rhi::RenderTarget& source,
                                           const rhi::Scissor& source_region,
                                           const rhi::RenderTarget& destination,
                                           rhi::Offset2D destination_offset) {
    if (!usable()) {
        return;
    }
    if (render_pass_active_) {
        throw std::runtime_error("copy_target_region: render pass is still active");
    }
    const auto* vulkan_source = &vulkan_cast<const VulkanRenderTarget>(source);
    const auto* vulkan_destination = &vulkan_cast<const VulkanRenderTarget>(destination);
    if (vulkan_source == vulkan_destination) {
        // One image cannot be in TRANSFER_SRC and TRANSFER_DST at the same time, and an in-image
        // copy needs GENERAL plus non-overlap checks. Reject it instead of guessing.
        throw std::invalid_argument(
            "copy_target_region: source and destination must be different targets");
    }
    if (vulkan_source->sample_count() != 1 || vulkan_destination->sample_count() != 1) {
        throw std::invalid_argument("copy_target_region: multisampled targets are not supported");
    }
    if (vulkan_source->format() != vulkan_destination->format()) {
        throw std::invalid_argument("copy_target_region: source and destination formats differ");
    }
    if (vulkan_source->presentable()) {
        // Swapchain images are created for color attachment (and transfer-dst) only; reading one
        // needs TRANSFER_SRC usage, which is a window/swapchain decision, not an RHI one.
        throw std::invalid_argument("copy_target_region: a swapchain image is not a copy source");
    }
    if (vulkan_destination->presentable()) {
        throw std::invalid_argument(
            "copy_target_region: a swapchain image is not a copy destination");
    }
    if (!region_fits(source_region, vulkan_source->extent())) {
        throw std::invalid_argument("copy_target_region: source region exceeds the target");
    }
    if (!offset_region_fits(destination_offset, source_region, vulkan_destination->extent())) {
        throw std::invalid_argument(
            "copy_target_region: destination offset plus region exceeds the target");
    }
    if (!vulkan_source->content_initialized()) {
        // A never-rendered target is still UNDEFINED, so there are no pixels to copy.
        throw std::runtime_error("copy_target_region: the source target has no rendered content");
    }

    if (!recording_) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer(copy region)");
        recording_ = true;
        record_timestamp_begin();
    }

    // The source is normally in TRANSFER_SRC with its write already available to a transfer read
    // (the end_render_pass() or previous-copy barrier), so only the destination is transitioned.
    // RHI 1.3: a target that was bound as a sampled texture is SHADER_READ_ONLY_OPTIMAL, so make it
    // available to the transfer stage first instead of assuming the transfer state.
    if (vulkan_source->image_layout() != VK_IMAGE_LAYOUT_UNDEFINED &&
        vulkan_source->image_layout() != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        const LayoutSource from = source_for_layout(vulkan_source->image_layout());
        record_image_transition(buffer_, vulkan_source->image(), vulkan_source->image_layout(),
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from.access,
                                VK_ACCESS_TRANSFER_READ_BIT, from.stage,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vulkan_source->mark_image_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    const VkImageLayout destination_previous =
        vulkan_destination->content_initialized()
            ? (vulkan_destination->image_layout() == VK_IMAGE_LAYOUT_UNDEFINED
                   ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                   : vulkan_destination->image_layout())
            : VK_IMAGE_LAYOUT_UNDEFINED;
    // A destination that was sampled is being overwritten (a write-after-read), so the barrier
    // waits on the shader read it may still be serving; an untouched destination waits on nothing.
    const LayoutSource destination_source = source_for_layout(destination_previous);
    record_image_transition(buffer_, vulkan_destination->image(), destination_previous,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destination_source.access,
                            VK_ACCESS_TRANSFER_WRITE_BIT, destination_source.stage,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffset = {source_region.x, source_region.y, 0};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel = 0;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = 1;
    region.dstOffset = {destination_offset.x, destination_offset.y, 0};
    region.extent = {source_region.width, source_region.height, 1};
    vkCmdCopyImage(buffer_, vulkan_source->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   vulkan_destination->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // The destination becomes readable exactly like an offscreen target after a render pass:
    // TRANSFER_SRC with its transfer write available to a transfer read.
    record_image_transition(buffer_, vulkan_destination->image(),
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
    vulkan_destination->mark_content_initialized();
    vulkan_destination->mark_image_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

} // namespace tessera::rendering::vulkan
