// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

namespace tessera::rendering::vulkan {

VulkanCommandList::~VulkanCommandList() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

void VulkanCommandList::begin_render_pass(const rhi::RenderTarget& target,
                                          const rhi::RenderTargetDesc& desc) {
    auto* vulkan_target = dynamic_cast<const VulkanRenderTarget*>(&target);
    if (vulkan_target == nullptr) {
        throw std::runtime_error("begin_render_pass: incompatible render target");
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer");
    recording_ = true;
    current_target_ = vulkan_target;

    // end_render_pass leaves the target in TRANSFER_SRC so copy_target_to_buffer can read it.
    // A Load frame must preserve that content, so it transitions from that layout instead of
    // discarding the image from UNDEFINED. Clear/DontCare may discard.
    const bool preserve = desc.load_op == rhi::LoadOp::Load;
    record_image_transition(
        buffer_, vulkan_target->image(),
        preserve ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, preserve ? VK_ACCESS_TRANSFER_READ_BIT : 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        preserve ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = vulkan_target->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = to_vk_load_op(desc.load_op);
    attachment.storeOp = to_vk_store_op(desc.store_op);
    attachment.clearValue.color.float32[0] = desc.clear_color.r;
    attachment.clearValue.color.float32[1] = desc.clear_color.g;
    attachment.clearValue.color.float32[2] = desc.clear_color.b;
    attachment.clearValue.color.float32[3] = desc.clear_color.a;

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset = {0, 0};
    rendering_info.renderArea.extent = {desc.extent.width, desc.extent.height};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &attachment;

    vkCmdBeginRendering(buffer_, &rendering_info);
    render_pass_active_ = true;
}

void VulkanCommandList::end_render_pass() {
    if (!render_pass_active_ || current_target_ == nullptr) {
        return;
    }

    vkCmdEndRendering(buffer_);
    record_image_transition(
        buffer_, current_target_->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    render_pass_active_ = false;
}

void VulkanCommandList::bind_pipeline(const rhi::Pipeline& pipeline) {
    auto* vulkan_pipeline = dynamic_cast<const VulkanPipeline*>(&pipeline);
    if (vulkan_pipeline == nullptr) {
        throw std::runtime_error("bind_pipeline: incompatible pipeline");
    }
    vkCmdBindPipeline(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_pipeline->handle());
    current_layout_ = vulkan_pipeline->layout();
}

void VulkanCommandList::bind_vertex_buffer(const rhi::Buffer& buffer) {
    auto* vulkan_buffer = dynamic_cast<const VulkanBuffer*>(&buffer);
    if (vulkan_buffer == nullptr) {
        throw std::runtime_error("bind_vertex_buffer: incompatible buffer");
    }
    const VkBuffer handle = vulkan_buffer->handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(buffer_, 0, 1, &handle, &offset);
}

void VulkanCommandList::bind_texture(std::uint32_t slot, const rhi::Texture& texture) {
    auto* vulkan_texture = dynamic_cast<const VulkanTexture*>(&texture);
    if (vulkan_texture == nullptr || current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("bind_texture: incompatible texture or missing pipeline");
    }
    const VkDescriptorSet descriptor_set = vulkan_texture->descriptor_set();
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, current_layout_, slot, 1,
                            &descriptor_set, 0, nullptr);
}

void VulkanCommandList::set_viewport(const rhi::Viewport& viewport) {
    VkViewport vk_viewport{};
    vk_viewport.x = viewport.x;
    vk_viewport.y = viewport.y;
    vk_viewport.width = viewport.width;
    vk_viewport.height = viewport.height;
    vk_viewport.minDepth = viewport.min_depth;
    vk_viewport.maxDepth = viewport.max_depth;
    vkCmdSetViewport(buffer_, 0, 1, &vk_viewport);
}

void VulkanCommandList::set_scissor(const rhi::Scissor& scissor) {
    VkRect2D vk_scissor{};
    vk_scissor.offset = {scissor.x, scissor.y};
    vk_scissor.extent = {scissor.width, scissor.height};
    vkCmdSetScissor(buffer_, 0, 1, &vk_scissor);
}

void VulkanCommandList::push_constants(const void* data, std::size_t size) {
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("push_constants: missing pipeline");
    }
    vkCmdPushConstants(buffer_, current_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<std::uint32_t>(size), data);
}

void VulkanCommandList::draw(std::uint32_t vertex_count, std::uint32_t first_vertex) {
    vkCmdDraw(buffer_, vertex_count, 1, first_vertex, 0);
}

void VulkanCommandList::copy_target_to_buffer(const rhi::RenderTarget& target,
                                              rhi::Buffer& buffer) {
    auto* vulkan_target = dynamic_cast<const VulkanRenderTarget*>(&target);
    auto* vulkan_buffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkan_target == nullptr || vulkan_buffer == nullptr) {
        throw std::runtime_error("copy_target_to_buffer: incompatible target or buffer");
    }

    if (!recording_) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer(copy)");
        recording_ = true;
    }

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {vulkan_target->extent().width, vulkan_target->extent().height, 1};

    vkCmdCopyImageToBuffer(buffer_, vulkan_target->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vulkan_buffer->handle(), 1, &copy);
}

void VulkanCommandList::submit() {
    if (!recording_ || submitted_) {
        return;
    }
    if (render_pass_active_) {
        throw std::runtime_error("submit: render pass is still active");
    }

    check_vk(vkEndCommandBuffer(buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer_;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    check_vk(vkCreateFence(device_, &fence_info, nullptr, &fence), "vkCreateFence");
    check_vk(vkQueueSubmit(queue_, 1, &submit_info, fence), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkDestroyFence(device_, fence, nullptr);

    submitted_ = true;
}

} // namespace tessera::rendering::vulkan
