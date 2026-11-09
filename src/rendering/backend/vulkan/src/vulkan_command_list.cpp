// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

namespace tessera::rendering::vulkan {

VulkanCommandList::~VulkanCommandList() {
    // A list dropped while the GPU still runs it must not free its pool; the device keeps it in
    // flight and recycles it once the fence signals.
    owner_.release_command_resources(resources_, submitted_ && !is_complete());
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

    // end_render_pass leaves offscreen targets in TRANSFER_SRC for readback and swapchain
    // images in PRESENT_SRC. A Load frame preserves that layout instead of discarding it.
    const bool preserve = desc.load_op == rhi::LoadOp::Load;
    const bool present = vulkan_target->presentable();
    record_image_transition(
        buffer_, vulkan_target->image(),
        preserve
            ? (present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        preserve && !present ? VK_ACCESS_TRANSFER_READ_BIT : 0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        preserve ? (present ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT)
                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
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
    const bool present = current_target_->presentable();
    record_image_transition(
        buffer_, current_target_->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, present ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        present ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT);
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
    if (vulkan_target->presentable()) {
        throw std::runtime_error("copy_target_to_buffer: swapchain images are not readable");
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

void VulkanCommandList::submit_async() {
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
    check_vk(vkQueueSubmit(queue_, 1, &submit_info, resources_.fence), "vkQueueSubmit");

    submitted_ = true;
}

// Blocking by contract: offscreen rendering, readback and golden tests rely on the result being
// complete on return. The UI frame path uses submit_async() instead.
void VulkanCommandList::submit() {
    submit_async();
    if (!submitted_ || complete_) {
        return;
    }
    check_vk(vkWaitForFences(device_, 1, &resources_.fence, VK_TRUE, UINT64_MAX),
             "vkWaitForFences");
    complete_ = true;
}

bool VulkanCommandList::is_complete() const {
    if (!submitted_) {
        return false;
    }
    if (!complete_) {
        // Anything but "still running" means the submission no longer uses the buffer; on a lost
        // device it never will, and reporting incomplete forever would hang pollers.
        complete_ = vkGetFenceStatus(device_, resources_.fence) != VK_NOT_READY;
    }
    return complete_;
}

bool VulkanCommandList::wait(std::uint64_t timeout_ms) {
    if (!submitted_) {
        return false;
    }
    if (complete_) {
        return true;
    }
    constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
    const std::uint64_t timeout_ns = timeout_ms > UINT64_MAX / kNanosecondsPerMillisecond
                                         ? UINT64_MAX
                                         : timeout_ms * kNanosecondsPerMillisecond;
    complete_ = vkWaitForFences(device_, 1, &resources_.fence, VK_TRUE, timeout_ns) != VK_TIMEOUT;
    return complete_;
}

} // namespace tessera::rendering::vulkan
