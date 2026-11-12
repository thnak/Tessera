// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan {

namespace {
// The synchronous submit is documented as offscreen/readback only, but even that path must not wait
// forever on a wedged device (ADR-021).
constexpr std::uint64_t kSyncSubmitWaitMs = 60'000;
constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
} // namespace

VulkanCommandList::~VulkanCommandList() {
    // A list dropped while the GPU still runs it must not free its pool; the device keeps it in
    // flight and recycles it once the fence signals. After a loss the device drops or destroys the
    // resources itself.
    owner_.release_command_resources(resources_, submitted_ && !is_complete());
    owner_.resource_destroyed();
}

bool VulkanCommandList::usable() const {
    return owner_.usable_epoch(epoch_);
}

void VulkanCommandList::require_usable(const char* what) const {
    if (!usable()) {
        throw std::runtime_error(std::string(what) +
                                 ": command list belongs to a lost device generation");
    }
}

void VulkanCommandList::begin_render_pass(const rhi::RenderTarget& target,
                                          const rhi::RenderTargetDesc& desc) {
    require_usable("begin_render_pass");
    const auto* vulkan_target = &vulkan_cast<const VulkanRenderTarget>(target);
    const std::uint32_t sample_count = vulkan_target->sample_count();
    if (desc.sample_count != sample_count) {
        throw std::invalid_argument(
            "begin_render_pass: descriptor sample count " + std::to_string(desc.sample_count) +
            " does not match target sample count " + std::to_string(sample_count));
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer");
    recording_ = true;
    current_target_ = vulkan_target;

    // end_render_pass leaves offscreen targets in TRANSFER_SRC for readback and swapchain
    // images in PRESENT_SRC. A Load frame preserves that layout instead of discarding it. A 4x
    // target renders into the multisample image (discarded after the resolve) and resolves into its
    // 1-sample image, which then becomes the readback source.
    const bool preserve = desc.load_op == rhi::LoadOp::Load;
    const bool present = vulkan_target->presentable();
    const bool multisampled = sample_count > 1;
    // loadOp=LOAD reads the existing color attachment, so the destination access must include
    // COLOR_ATTACHMENT_READ (otherwise validation reports a READ_AFTER_WRITE hazard).
    //
    // A preserving pass can only assume the layout the target has after a render: PRESENT_SRC for
    // swapchain images, TRANSFER_SRC for offscreen readback targets, COLOR_ATTACHMENT_OPTIMAL for
    // multisampled images. A fresh target image is UNDEFINED (including after a swapchain or
    // target recreation), so the old layout follows the target's history.
    const bool had_content = vulkan_target->content_initialized();
    const VkImageLayout previous_layout =
        !preserve || !had_content
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : (multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            : (present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                       : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
    const bool preserving_existing = preserve && had_content;
    const VkAccessFlags color_access =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (preserve ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : 0);
    const VkPipelineStageFlags source_stage =
        multisampled ? (preserving_existing ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                     : (preserving_existing ? (present ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                       : VK_PIPELINE_STAGE_TRANSFER_BIT)
                                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    // The submission that renders an acquired swapchain image waits on its acquire semaphore at
    // COLOR_ATTACHMENT_OUTPUT (vulkan_command_list.cpp submit_async), so anything that executes
    // earlier in that submission - including this layout transition - is not ordered after the
    // acquisition. Synchronization validation reports that as WRITE_AFTER_READ; a barrier scoped to
    // the stage the wait covers is the execution dependency that fixes it.
    const bool waits_for_acquire = present && vulkan_target->frame_sync_pending();
    // The cast keeps both arms the same VkPipelineStageFlags type; mixing the enumerator with the
    // flags mask is -Wextra on GCC and warnings are errors.
    const VkPipelineStageFlags acquire_stage =
        static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    record_image_transition(
        buffer_, vulkan_target->image(), previous_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        multisampled ? (preserving_existing ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0)
                     : (preserving_existing && !present ? VK_ACCESS_TRANSFER_READ_BIT : 0),
        color_access, waits_for_acquire ? acquire_stage : source_stage,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (multisampled) {
        record_image_transition(
            buffer_, vulkan_target->resolve_image(), VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = vulkan_target->view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // A Load pass on an image with no rendered content (fresh target, recreated swapchain image)
    // would read undefined pixels; clear it instead so the result is defined.
    attachment.loadOp =
        (preserve && !had_content) ? VK_ATTACHMENT_LOAD_OP_CLEAR : to_vk_load_op(desc.load_op);
    // For MSAA the multisample image is discarded after the resolve; only the resolved image is
    // stored. 1x targets keep the caller's store op.
    attachment.storeOp =
        multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : to_vk_store_op(desc.store_op);
    if (multisampled) {
        attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        attachment.resolveImageView = vulkan_target->resolve_view();
        attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
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
    if (!usable()) {
        // The device was lost mid-recording; the command buffer is abandoned with the device.
        render_pass_active_ = false;
        return;
    }

    vkCmdEndRendering(buffer_);
    const bool present = current_target_->presentable();
    if (current_target_->sample_count() > 1) {
        // The resolve wrote the 1-sample image during the pass; the multisample image is discarded
        // (DONT_CARE). The resolved image becomes the readback source.
        record_image_transition(
            buffer_, current_target_->resolve_image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        record_image_transition(
            buffer_, current_target_->image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            present ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, present ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            present ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    render_pass_active_ = false;
    // The target now holds a known layout (PRESENT_SRC / TRANSFER_SRC / COLOR_ATTACHMENT_OPTIMAL),
    // so a later preserving pass may transition from that layout instead of UNDEFINED.
    current_target_->mark_content_initialized();
}

void VulkanCommandList::bind_pipeline(const rhi::Pipeline& pipeline) {
    require_usable("bind_pipeline");
    const auto& vulkan_pipeline = vulkan_cast<const VulkanPipeline>(pipeline);
    if (vulkan_pipeline.epoch() != epoch_) {
        throw std::runtime_error("bind_pipeline: pipeline belongs to a lost device generation");
    }
    if (current_target_ != nullptr && render_pass_active_ &&
        vulkan_pipeline.sample_count() != current_target_->sample_count()) {
        throw std::runtime_error("bind_pipeline: pipeline sample count " +
                                 std::to_string(vulkan_pipeline.sample_count()) +
                                 " does not match target sample count " +
                                 std::to_string(current_target_->sample_count()));
    }
    current_pipeline_sample_count_ = vulkan_pipeline.sample_count();
    vkCmdBindPipeline(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_pipeline.handle());
    current_layout_ = vulkan_pipeline.layout();
}

void VulkanCommandList::bind_vertex_buffer(const rhi::Buffer& buffer) {
    require_usable("bind_vertex_buffer");
    const VkBuffer handle = vulkan_cast<const VulkanBuffer>(buffer).handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(buffer_, 0, 1, &handle, &offset);
}

void VulkanCommandList::bind_texture(std::uint32_t slot, const rhi::Texture& texture) {
    require_usable("bind_texture");
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("bind_texture: missing pipeline");
    }
    const VkDescriptorSet descriptor_set =
        vulkan_cast<const VulkanTexture>(texture).descriptor_set();
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, current_layout_, slot, 1,
                            &descriptor_set, 0, nullptr);
}

void VulkanCommandList::set_viewport(const rhi::Viewport& viewport) {
    if (!usable()) {
        return;
    }
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
    if (!usable()) {
        return;
    }
    VkRect2D vk_scissor{};
    vk_scissor.offset = {scissor.x, scissor.y};
    vk_scissor.extent = {scissor.width, scissor.height};
    vkCmdSetScissor(buffer_, 0, 1, &vk_scissor);
}

void VulkanCommandList::push_constants(const void* data, std::size_t size) {
    if (!usable()) {
        return;
    }
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("push_constants: missing pipeline");
    }
    vkCmdPushConstants(buffer_, current_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<std::uint32_t>(size), data);
}

void VulkanCommandList::draw(std::uint32_t vertex_count, std::uint32_t first_vertex) {
    if (!usable()) {
        return;
    }
    vkCmdDraw(buffer_, vertex_count, 1, first_vertex, 0);
}

void VulkanCommandList::copy_target_to_buffer(const rhi::RenderTarget& target,
                                              rhi::Buffer& buffer) {
    if (!usable()) {
        return;
    }
    const auto* vulkan_target = &vulkan_cast<const VulkanRenderTarget>(target);
    auto* vulkan_buffer = &vulkan_cast<VulkanBuffer>(buffer);
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

    vkCmdCopyImageToBuffer(buffer_, vulkan_target->readback_image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vulkan_buffer->handle(), 1, &copy);
}

void VulkanCommandList::blit_target(const rhi::RenderTarget& source,
                                    const rhi::RenderTarget& destination, rhi::Filter filter) {
    if (!usable()) {
        return;
    }
    const auto* vulkan_source = &vulkan_cast<const VulkanRenderTarget>(source);
    const auto* vulkan_destination = &vulkan_cast<const VulkanRenderTarget>(destination);
    if (vulkan_source->sample_count() != 1 || vulkan_destination->sample_count() != 1) {
        throw std::runtime_error("blit_target: multisampled targets are not supported");
    }
    if (vulkan_source->presentable()) {
        throw std::runtime_error("blit_target: swapchain images are not a blit source");
    }
    if (!vulkan_source->content_initialized()) {
        // The source has never been rendered, so there is nothing to resample and its image is
        // still in UNDEFINED layout.
        throw std::runtime_error("blit_target: the source target has no rendered content");
    }

    if (!recording_) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer(blit)");
        recording_ = true;
    }
    if (render_pass_active_) {
        throw std::runtime_error("blit_target: render pass is still active");
    }

    // end_render_pass() left the offscreen source in TRANSFER_SRC_OPTIMAL with its color write made
    // available to a transfer read, so only the destination needs a barrier. The destination was
    // presented before (PRESENT_SRC) or is a fresh swapchain image (UNDEFINED).
    const bool destination_presentable = vulkan_destination->presentable();
    const bool destination_has_content = vulkan_destination->content_initialized();
    const bool waits_for_acquire =
        destination_presentable && vulkan_destination->frame_sync_pending();
    // A previously written destination is in the layout the last write left it in: PRESENT_SRC for
    // a swapchain image, TRANSFER_SRC for an offscreen target (end_render_pass's post-render
    // layout).
    const VkImageLayout destination_previous = !destination_has_content ? VK_IMAGE_LAYOUT_UNDEFINED
                                               : destination_presentable
                                                   ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                   : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // The submission waits on the acquire semaphore at TRANSFER (see submit_async), so the layout
    // transition that first writes the acquired image must be scoped to that stage as well.
    const VkPipelineStageFlags acquire_stage =
        static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TRANSFER_BIT);
    record_image_transition(
        buffer_, vulkan_destination->image(), destination_previous,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        waits_for_acquire ? acquire_stage
                          : static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
        VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {static_cast<std::int32_t>(vulkan_source->extent().width),
                            static_cast<std::int32_t>(vulkan_source->extent().height), 1};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.mipLevel = 0;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {static_cast<std::int32_t>(vulkan_destination->extent().width),
                            static_cast<std::int32_t>(vulkan_destination->extent().height), 1};
    vkCmdBlitImage(buffer_, vulkan_source->readback_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   vulkan_destination->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   filter == rhi::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    record_image_transition(
        buffer_, vulkan_destination->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        destination_presentable ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, destination_presentable ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        destination_presentable ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                : VK_PIPELINE_STAGE_TRANSFER_BIT);

    // The submission is now a present of this destination: submit_async() derives the acquire wait
    // and the present semaphore from the target that was written, exactly as it does after a render
    // pass into a swapchain image.
    current_target_ = vulkan_destination;
    present_written_by_transfer_ = destination_presentable;
    vulkan_destination->mark_content_initialized();
}

void VulkanCommandList::submit_async() {
    if (!recording_ || submitted_) {
        return;
    }
    if (!usable()) {
        submitted_ = true;
        return;
    }
    if (render_pass_active_) {
        throw std::runtime_error("submit: render pass is still active");
    }
    if (owner_.consume_injected_fault(InjectedDeviceFault::DeviceLostOnSubmit)) {
        owner_.mark_device_lost();
        submitted_ = true;
        return;
    }

    const VkResult end_result = vkEndCommandBuffer(buffer_);
    if (end_result == VK_ERROR_DEVICE_LOST) {
        owner_.mark_device_lost();
        submitted_ = true;
        return;
    }
    check_vk(end_result, "vkEndCommandBuffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer_;

    // Rendering to a swapchain image participates in the swapchain's frame pacing: the submission
    // waits on the slot's binary acquire semaphore (image acquisition never blocks the CPU) and
    // signals the timeline value that makes the slot reusable by a counter query
    // (docs/design/gpu/frames.md section 5.1). Offscreen targets publish no binding and keep the
    // blocking fence path.
    const bool presentable = current_target_ != nullptr && current_target_->presentable();
    std::array<VkSemaphore, 2> wait_semaphores{};
    std::array<VkPipelineStageFlags, 2> wait_stages{};
    std::array<std::uint64_t, 2> wait_values{};
    std::array<VkSemaphore, 2> signal_semaphores{};
    std::array<std::uint64_t, 2> signal_values{};
    std::uint32_t wait_count = 0;
    std::uint32_t signal_count = 0;

    bool timeline_signaled = false;
    if (presentable && current_target_->frame_sync_pending()) {
        const FrameSyncBinding& binding = current_target_->frame_sync();
        if (binding.acquire != VK_NULL_HANDLE) {
            // The acquisition is waited for before the (first) write to the image: the color
            // attachment output of a render pass, or the transfer stage of a resample blit. No
            // earlier stage touches the image.
            wait_stages[wait_count] =
                present_written_by_transfer_
                    ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TRANSFER_BIT)
                    : static_cast<VkPipelineStageFlags>(
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            wait_semaphores[wait_count++] = binding.acquire;
        }
        if (binding.timeline != VK_NULL_HANDLE) {
            signal_semaphores[signal_count] = binding.timeline;
            signal_values[signal_count] = binding.value;
            ++signal_count;
            timeline_signaled = true;
        }
    }
    // A second render to the same image before present must not signal its binary semaphore again.
    const VkSemaphore render_finished = presentable && !current_target_->render_pending()
                                            ? current_target_->render_finished_semaphore()
                                            : VK_NULL_HANDLE;
    if (render_finished != VK_NULL_HANDLE) {
        signal_semaphores[signal_count++] = render_finished;
    }

    VkTimelineSemaphoreSubmitInfo timeline_info{};
    if (timeline_signaled) {
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        // Binary semaphores in the same submission ignore their value entry, so the zeroed waits
        // are the documented encoding for them.
        timeline_info.waitSemaphoreValueCount = wait_count;
        timeline_info.pWaitSemaphoreValues = wait_count > 0 ? wait_values.data() : nullptr;
        timeline_info.signalSemaphoreValueCount = signal_count;
        timeline_info.pSignalSemaphoreValues = signal_values.data();
        submit_info.pNext = &timeline_info;
    }
    if (wait_count > 0) {
        submit_info.waitSemaphoreCount = wait_count;
        submit_info.pWaitSemaphores = wait_semaphores.data();
        submit_info.pWaitDstStageMask = wait_stages.data();
    }
    if (signal_count > 0) {
        submit_info.signalSemaphoreCount = signal_count;
        submit_info.pSignalSemaphores = signal_semaphores.data();
    }

    const VkResult submit_result = vkQueueSubmit(queue_, 1, &submit_info, resources_.fence);
    if (submit_result == VK_ERROR_DEVICE_LOST) {
        // D7: loss is state, not an exception. The caller polls Device::status() and recovers.
        owner_.mark_device_lost();
        submitted_ = true;
        return;
    }
    check_vk(submit_result, "vkQueueSubmit");
    if (render_finished != VK_NULL_HANDLE) {
        current_target_->set_render_pending(true);
    }
    if (timeline_signaled) {
        // The value is now on the queue; the swapchain frees the slot when the counter reaches it.
        current_target_->mark_frame_submitted();
    }

    submitted_ = true;
}

// Blocking by contract: offscreen rendering, readback and golden tests rely on the result being
// complete on return. The UI frame path uses submit_async() instead. The wait is bounded (ADR-021):
// a device loss returns immediately as Lost state, a wedged fence reports incomplete instead of
// waiting forever.
void VulkanCommandList::submit() {
    submit_async();
    if (!submitted_ || complete_) {
        return;
    }
    if (!usable()) {
        return;
    }
    const std::uint64_t timeout_ns = kSyncSubmitWaitMs * kNanosecondsPerMillisecond;
    const VkResult result = vkWaitForFences(device_, 1, &resources_.fence, VK_TRUE, timeout_ns);
    if (result == VK_ERROR_DEVICE_LOST) {
        owner_.mark_device_lost();
        return;
    }
    if (result == VK_SUCCESS) {
        complete_ = true;
    }
}

bool VulkanCommandList::is_complete() const {
    if (!submitted_ || !usable()) {
        return false;
    }
    if (complete_) {
        return true;
    }
    if (owner_.consume_injected_fault(InjectedDeviceFault::DeviceLostOnFenceWait)) {
        owner_.mark_device_lost();
        return false;
    }
    // Anything but "still running" means the submission no longer uses the buffer; on a lost
    // device it never will, and reporting incomplete forever would hang pollers.
    const VkResult status = vkGetFenceStatus(device_, resources_.fence);
    if (status == VK_ERROR_DEVICE_LOST) {
        owner_.mark_device_lost();
        return false;
    }
    complete_ = status != VK_NOT_READY;
    return complete_;
}

bool VulkanCommandList::wait(std::uint64_t timeout_ms) {
    if (!submitted_ || !usable()) {
        return false;
    }
    if (complete_) {
        return true;
    }
    if (owner_.consume_injected_fault(InjectedDeviceFault::DeviceLostOnFenceWait)) {
        owner_.mark_device_lost();
        return false;
    }
    const std::uint64_t timeout_ns = timeout_ms > UINT64_MAX / kNanosecondsPerMillisecond
                                         ? UINT64_MAX
                                         : timeout_ms * kNanosecondsPerMillisecond;
    const VkResult result = vkWaitForFences(device_, 1, &resources_.fence, VK_TRUE, timeout_ns);
    if (result == VK_ERROR_DEVICE_LOST) {
        owner_.mark_device_lost();
        return false;
    }
    complete_ = result == VK_SUCCESS;
    return complete_;
}

} // namespace tessera::rendering::vulkan
