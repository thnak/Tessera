// SPDX-License-Identifier: MIT
//
// OffscreenRenderer pipeline creation, command recording and submission (split from
// offscreen_renderer.cpp, which keeps instance/device/target setup, readback and teardown).

#include "offscreen_renderer_internal.hpp"
#include <tessera/rendering/vulkan/offscreen_renderer.hpp>

#include <cstdint>

namespace {

// glslc -mfmt=c emits a bare initializer list: { word, word, ... }.
alignas(4) const std::uint32_t kFullscreenVertSpv[] =
#include "fullscreen_vert_spv.h"
    ;

alignas(4) const std::uint32_t kFullscreenFragSpv[] =
#include "fullscreen_frag_spv.h"
    ;

} // namespace

namespace tessera::rendering::vulkan {

using offscreen_detail::check_vk;
using offscreen_detail::kColorFormat;

VkShaderModule OffscreenRenderer::make_shader(const std::uint32_t* code,
                                              std::size_t byte_count) const {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = byte_count;
    create_info.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    check_vk(vkCreateShaderModule(device_, &create_info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void OffscreenRenderer::create_pipeline() {
    VkShaderModule vertex = make_shader(kFullscreenVertSpv, sizeof(kFullscreenVertSpv));
    VkShaderModule fragment = make_shader(kFullscreenFragSpv, sizeof(kFullscreenFragSpv));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_),
             "vkCreatePipelineLayout");

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_FALSE;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkFormat color_format = kColorFormat;
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    check_vk(
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_),
        "vkCreateGraphicsPipelines");

    vkDestroyShaderModule(device_, vertex, nullptr);
    vkDestroyShaderModule(device_, fragment, nullptr);
}

void OffscreenRenderer::create_command_objects() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    check_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_),
             "vkCreateCommandPool");

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer_),
             "vkAllocateCommandBuffers");
}

void OffscreenRenderer::record_and_submit() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command_buffer_, &begin_info), "vkBeginCommandBuffer");

    const auto image_barrier = [&](VkImageLayout old_layout, VkImageLayout new_layout,
                                   VkAccessFlags source_access, VkAccessFlags destination_access,
                                   VkPipelineStageFlags source_stage,
                                   VkPipelineStageFlags destination_stage) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;

        vkCmdPipelineBarrier(command_buffer_, source_stage, destination_stage, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
    };

    image_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = image_view_;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color.float32[0] = 0.02f;
    color_attachment.clearValue.color.float32[1] = 0.02f;
    color_attachment.clearValue.color.float32[2] = 0.03f;
    color_attachment.clearValue.color.float32[3] = 1.0f;

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset = {0, 0};
    rendering_info.renderArea.extent = {config_.width, config_.height};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(command_buffer_, &rendering_info);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const float resolution[2] = {static_cast<float>(config_.width),
                                 static_cast<float>(config_.height)};
    vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(resolution), resolution);

    VkViewport viewport{};
    viewport.width = static_cast<float>(config_.width);
    viewport.height = static_cast<float>(config_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {config_.width, config_.height};
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

    vkCmdDraw(command_buffer_, 3, 1, 0, 0);
    vkCmdEndRendering(command_buffer_);

    image_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {config_.width, config_.height, 1};

    vkCmdCopyImageToBuffer(command_buffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buffer_, 1, &copy);

    check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    check_vk(vkCreateFence(device_, &fence_info, nullptr, &fence), "vkCreateFence");
    check_vk(vkQueueSubmit(queue_, 1, &submit_info, fence), "vkQueueSubmit");
    check_vk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkDestroyFence(device_, fence, nullptr);
}

} // namespace tessera::rendering::vulkan
