// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"
#include "vulkan_scope_guard.hpp"

#include <vector>

namespace tessera::rendering::vulkan {

std::unique_ptr<rhi::Pipeline> VulkanDevice::create_pipeline(const rhi::PipelineDesc& desc) {
    require_ready("create_pipeline");
    const std::uint32_t sample_count = checked_sample_count(desc.sample_count, "create_pipeline");
    if (sample_count > 1 && !supports_sample_count(desc.color_format, sample_count)) {
        throw std::runtime_error("create_pipeline: " + std::to_string(sample_count) +
                                 "x MSAA is not supported for this target format on this device");
    }
    if (desc.vertex_layout.stride == 0 && desc.vertex_layout.attribute_count != 0) {
        throw std::invalid_argument(
            "create_pipeline: a vertex attribute layout needs a non-zero stride");
    }
    // Dual-source blend factors are only legal with the dualSrcBlend feature, so a device without
    // it would either fail validation or produce an undefined blend. Report the request instead of
    // mapping it to another mode: the caller's documented fallback is grayscale coverage, which is
    // a different pipeline, not a different blend state on this one (RHI 1.5, issue #471).
    if (desc.blend == rhi::BlendMode::DualSource && !dual_source_blend_) {
        throw std::runtime_error(
            "create_pipeline: dual-source blending is not supported on this device");
    }
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = desc.vertex_spirv_size;
    shader_info.pCode = desc.vertex_spirv;
    check_vk(vkCreateShaderModule(device_, &shader_info, nullptr, &vertex_module),
             "vkCreateShaderModule(vertex)");
    // Modules are only needed while the pipeline is created, so they are destroyed on every exit.
    ScopeGuard vertex_guard([&] { vkDestroyShaderModule(device_, vertex_module, nullptr); });

    shader_info.codeSize = desc.fragment_spirv_size;
    shader_info.pCode = desc.fragment_spirv;
    check_vk(vkCreateShaderModule(device_, &shader_info, nullptr, &fragment_module),
             "vkCreateShaderModule(fragment)");
    ScopeGuard fragment_guard([&] { vkDestroyShaderModule(device_, fragment_module, nullptr); });

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = desc.push_constant_size;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // A pipeline whose shader indexes a sampled-image array declares the array layout; every other
    // pipeline keeps the one-combined-image-sampler layout (RHI 1.7, issue #480 part 2). The choice
    // is the descriptor's, so a caller that wants the array path asks for it explicitly and a
    // device without the capability refuses it instead of building a pipeline that cannot bind.
    VkDescriptorSetLayout set_layout = descriptor_set_layout_;
    if (desc.texture_array) {
        if (texture_array_layout_ == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "create_pipeline: texture arrays are not supported on this device");
        }
        set_layout = texture_array_layout_;
    }
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = desc.push_constant_size > 0 ? 1 : 0;
    layout_info.pPushConstantRanges = desc.push_constant_size > 0 ? &push_range : nullptr;

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout),
             "vkCreatePipelineLayout");
    ScopeGuard layout_guard([&] { vkDestroyPipelineLayout(device_, pipeline_layout, nullptr); });

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.vertex_layout.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attributes(desc.vertex_layout.attribute_count);
    for (std::uint32_t i = 0; i < desc.vertex_layout.attribute_count; ++i) {
        const rhi::VertexAttribute& source = desc.vertex_layout.attributes[i];
        attributes[i].location = source.location;
        attributes[i].binding = 0;
        attributes[i].format = to_vk_vertex_format(source.format);
        attributes[i].offset = source.offset;
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // An empty layout (fullscreen.vert builds its triangle from gl_VertexIndex, so glass_blur has
    // stride 0) declares no vertex binding at all: a binding with no attributes is legal but makes
    // drivers expect a buffer, and the draw then has nothing to bind.
    const bool has_vertex_layout = desc.vertex_layout.stride != 0;
    vertex_input.vertexBindingDescriptionCount = has_vertex_layout ? 1 : 0;
    vertex_input.pVertexBindingDescriptions = has_vertex_layout ? &binding : nullptr;
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

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
    multisample.rasterizationSamples = to_vk_sample_count(sample_count);

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend == rhi::BlendMode::DualSource) {
        // Per-channel coverage (RHI 1.5, issue #471): `src0 * src1 + dst * (1 - src1)`, where src1
        // is the fragment shader's second output at location 0 index 1. The SRC1 colour factors are
        // used for the alpha attachment too, so one coverage value drives all four channels exactly
        // as the RHI contract states; the coverage alpha is not a separate weight. These factors
        // are only legal with VkPhysicalDeviceFeatures::dualSrcBlend enabled, which is why the
        // request is refused above instead of reaching the driver.
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (desc.blend == rhi::BlendMode::Alpha) {
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    const VkFormat color_format = to_vk_format(desc.color_format);
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
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    check_vk(
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
        "vkCreateGraphicsPipelines");
    ScopeGuard pipeline_guard([&] { vkDestroyPipeline(device_, pipeline, nullptr); });

    // The wrapper owns the pipeline and layout from here; if allocating it throws, the guards
    // above still release them.
    resource_created();
    auto result = std::make_unique<VulkanPipeline>(this, epoch_, device_, pipeline, pipeline_layout,
                                                   sample_count);
    pipeline_guard.dismiss();
    layout_guard.dismiss();
    return result;
}

} // namespace tessera::rendering::vulkan
