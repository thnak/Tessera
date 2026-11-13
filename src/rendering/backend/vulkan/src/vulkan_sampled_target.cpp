// SPDX-License-Identifier: MIT
//
// CommandList::bind_sampled_target on Vulkan (RHI 1.3): a render target becomes a texture the
// bound pipeline can read, with the layout transition and the descriptor set the sampler needs.
// This is the capability the glass composite draws with (docs/design/gpu/offscreen-layers.md
// section 5, issue #254); the two-pass blur reads a pooled target, then reads the first pass's
// target while compositing into the frame.
//
// Layout bookkeeping: a target that holds content is in TRANSFER_SRC_OPTIMAL (the state
// end_render_pass(), copy_target_region() and blit_target() leave it in) or in
// SHADER_READ_ONLY_OPTIMAL (the state a previous bind left it in). This file only has to reach
// SHADER_READ_ONLY; every other reader transitions back from whichever state it finds
// (vulkan_command_list.cpp, vulkan_command_list_copy.cpp), so a target can alternate between
// "copied into", "sampled" and "read back" without the RHI exposing a layout.
//
// The transition is recorded outside a render pass by contract: inside a dynamic-rendering
// instance the transfer stage is not a legal barrier scope, so the barrier that chains from the
// transfer read of a copy has to be recorded before the pass opens.

#include "vulkan_internal.hpp"
#include "vulkan_scope_guard.hpp"

#include <stdexcept>

namespace tessera::rendering::vulkan {

LayoutSource source_for_layout(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return LayoutSource{static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TRANSFER_BIT),
                            static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_READ_BIT)};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return LayoutSource{static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TRANSFER_BIT),
                            static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_WRITE_BIT)};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return LayoutSource{
            static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
            static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return LayoutSource{
            static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT),
            static_cast<VkAccessFlags>(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
    default:
        break;
    }
    return LayoutSource{};
}

VkDescriptorSet VulkanDevice::sampled_descriptor(const VulkanRenderTarget& target) {
    if (target.sampled_descriptor_set() != VK_NULL_HANDLE) {
        return target.sampled_descriptor_set();
    }
    // One sampler per target, built from the state the target was created with. No mip levels, so
    // no LOD bias or anisotropy: a sampled target is read at level 0 with exactly the filter and
    // address mode its descriptor asked for.
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk_filter(target.filter());
    sampler_info.minFilter = sampler_info.magFilter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = to_vk_address(target.address());
    sampler_info.addressModeV = sampler_info.addressModeU;
    sampler_info.addressModeW = sampler_info.addressModeU;
    sampler_info.maxLod = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    check_vk(vkCreateSampler(device_, &sampler_info, nullptr, &sampler), "vkCreateSampler(target)");
    ScopeGuard sampler_guard([&] { vkDestroySampler(device_, sampler, nullptr); });

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_set_layout_;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &descriptor_set),
             "vkAllocateDescriptorSets(target)");
    ScopeGuard set_guard(
        [&] { vkFreeDescriptorSets(device_, descriptor_pool_, 1, &descriptor_set); });

    VkDescriptorImageInfo image{};
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.imageView = target.sampled_view();
    image.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // The target owns the pair from here; its destructor frees them.
    target.set_sampled_descriptor(sampler, descriptor_set, descriptor_pool_);
    sampler_guard.dismiss();
    set_guard.dismiss();
    return descriptor_set;
}

void VulkanCommandList::bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) {
    if (!usable()) {
        return;
    }
    if (render_pass_active_) {
        // A dynamic-rendering instance cannot scope a barrier to the transfer stage, so the layout
        // transition a sampled bind records would be invalid here.
        throw std::logic_error("bind_sampled_target: render pass is still active");
    }
    const auto* vulkan_target = &vulkan_cast<const VulkanRenderTarget>(target);
    if (vulkan_target->presentable()) {
        throw std::invalid_argument(
            "bind_sampled_target: a swapchain image is not samplable (it is created for color "
            "attachment/transfer-dst; sampling one is a swapchain-usage decision)");
    }
    if (!vulkan_target->content_initialized()) {
        // A never-written target is still UNDEFINED: there are no pixels to sample.
        throw std::invalid_argument("bind_sampled_target: the target has no rendered content");
    }
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("bind_sampled_target: missing pipeline");
    }

    if (!recording_) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(buffer_, &begin_info),
                 "vkBeginCommandBuffer(sampled target)");
        recording_ = true;
        record_timestamp_begin();
    }

    // The readable image is the 1-sample one (the resolve image for a 4x target).
    if (vulkan_target->image_layout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        const VkImageLayout previous = vulkan_target->image_layout() == VK_IMAGE_LAYOUT_UNDEFINED
                                           ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                           : vulkan_target->image_layout();
        const LayoutSource from = source_for_layout(previous);
        record_image_transition(buffer_, vulkan_target->readback_image(), previous,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, from.access,
                                VK_ACCESS_SHADER_READ_BIT, from.stage,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vulkan_target->mark_image_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    const VkDescriptorSet descriptor_set = owner_.sampled_descriptor(*vulkan_target);
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, current_layout_, slot, 1,
                            &descriptor_set, 0, nullptr);
    sampled_target_ = vulkan_target;
}

} // namespace tessera::rendering::vulkan
