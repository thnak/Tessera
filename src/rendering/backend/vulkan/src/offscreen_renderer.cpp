// SPDX-License-Identifier: MIT

#include <tessera/rendering/vulkan/offscreen_renderer.hpp>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <stb_image_write.h>

namespace {

// glslc -mfmt=c emits a bare initializer list: { word, word, ... }.
alignas(4) const std::uint32_t kFullscreenVertSpv[] =
#include "fullscreen_vert_spv.h"
    ;

alignas(4) const std::uint32_t kFullscreenFragSpv[] =
#include "fullscreen_frag_spv.h"
    ;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult=" +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* /*user_data*/) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        std::cerr << "[vulkan] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

bool has_instance_extension(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool has_layer(const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> properties(count);
    if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkLayerProperties& property : properties) {
        if (std::strcmp(property.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace tessera::rendering::vulkan {

OffscreenRenderer::OffscreenRenderer(OffscreenConfig config)
    : config_(std::move(config)) {}

OffscreenRenderer::~OffscreenRenderer() {
    destroy();
}

void OffscreenRenderer::render_to_png() {
    create_instance();
    pick_device();
    create_device();
    create_target();
    create_pipeline();
    create_command_objects();
    record_and_submit();
    write_png();
}

void OffscreenRenderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Tessera Vulkan Offscreen PoC";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.pEngineName = "Tessera";
    app_info.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    app_info.apiVersion = VK_API_VERSION_1_3;

    const bool portability_available =
        has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    std::vector<const char*> extensions;
    if (portability_available) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    const bool validation_available = has_layer("VK_LAYER_KHRONOS_validation");
    const bool validation_enabled = config_.enable_validation && validation_available;
    if (config_.enable_validation && !validation_available) {
        std::cerr << "[vulkan] warning: validation layer not available; continuing without it\n";
    }
    if (validation_enabled) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    if (portability_available) {
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    check_vk(vkCreateInstance(&create_info, nullptr, &instance_), "vkCreateInstance");

    if (validation_enabled) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
            messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messenger_info.pfnUserCallback = debug_callback;
            check_vk(create_messenger(instance_, &messenger_info, nullptr, &debug_messenger_),
                     "vkCreateDebugUtilsMessengerEXT");
        }
    }
}

void OffscreenRenderer::pick_device() {
    std::uint32_t device_count = 0;
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr),
             "vkEnumeratePhysicalDevices");
    if (device_count == 0) {
        throw std::runtime_error("no Vulkan physical device found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    check_vk(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()),
             "vkEnumeratePhysicalDevices");

    int chosen = -1;
    if (config_.device_index >= 0) {
        if (static_cast<std::uint32_t>(config_.device_index) >= device_count) {
            throw std::runtime_error("requested Vulkan device index is out of range");
        }
        chosen = config_.device_index;
    } else {
        for (std::uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                chosen = static_cast<int>(i);
                break;
            }
        }
        if (chosen < 0) {
            chosen = 0;
        }
    }

    physical_device_ = devices[static_cast<std::size_t>(chosen)];

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);
    std::cout << "Vulkan device: " << properties.deviceName << " (API "
              << VK_VERSION_MAJOR(properties.apiVersion) << "."
              << VK_VERSION_MINOR(properties.apiVersion) << "."
              << VK_VERSION_PATCH(properties.apiVersion) << ")\n";

    if (properties.apiVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("Tessera requires a Vulkan 1.3 device");
    }

    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, queues.data());

    bool found_graphics = false;
    for (std::uint32_t i = 0; i < queue_count; ++i) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            found_graphics = true;
            break;
        }
    }
    if (!found_graphics) {
        throw std::runtime_error("no graphics queue family found");
    }
}

void OffscreenRenderer::create_device() {
    VkPhysicalDeviceVulkan13Features supported_13{};
    supported_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 supported_2{};
    supported_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_2.pNext = &supported_13;
    vkGetPhysicalDeviceFeatures2(physical_device_, &supported_2);

    if (!supported_13.dynamicRendering) {
        throw std::runtime_error("device does not support Vulkan 1.3 dynamic rendering");
    }

    VkPhysicalDeviceVulkan13Features enabled_13{};
    enabled_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled_13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures2 enabled_2{};
    enabled_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled_2.pNext = &enabled_13;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &enabled_2;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;

    check_vk(vkCreateDevice(physical_device_, &create_info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

std::uint32_t OffscreenRenderer::find_memory_type(std::uint32_t type_bits,
                                                  VkMemoryPropertyFlags required) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory);

    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    throw std::runtime_error("no suitable Vulkan memory type");
}

void OffscreenRenderer::create_target() {
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kColorFormat, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0 ||
        (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0) {
        throw std::runtime_error("VK_FORMAT_R8G8B8A8_UNORM cannot be used as an offscreen target");
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kColorFormat;
    image_info.extent = {config_.width, config_.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_vk(vkCreateImage(device_, &image_info, nullptr, &image_), "vkCreateImage");

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device_, image_, &image_requirements);

    VkMemoryAllocateInfo image_allocate{};
    image_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_allocate.allocationSize = image_requirements.size;
    image_allocate.memoryTypeIndex =
        find_memory_type(image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk(vkAllocateMemory(device_, &image_allocate, nullptr, &image_memory_),
             "vkAllocateMemory(image)");
    check_vk(vkBindImageMemory(device_, image_, image_memory_, 0), "vkBindImageMemory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kColorFormat;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    check_vk(vkCreateImageView(device_, &view_info, nullptr, &image_view_), "vkCreateImageView");

    const VkDeviceSize readback_size =
        static_cast<VkDeviceSize>(config_.width) * static_cast<VkDeviceSize>(config_.height) * 4;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = readback_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &readback_buffer_),
             "vkCreateBuffer(readback)");

    VkMemoryRequirements buffer_requirements{};
    vkGetBufferMemoryRequirements(device_, readback_buffer_, &buffer_requirements);

    VkMemoryAllocateInfo buffer_allocate{};
    buffer_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_allocate.allocationSize = buffer_requirements.size;
    buffer_allocate.memoryTypeIndex = find_memory_type(buffer_requirements.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk(vkAllocateMemory(device_, &buffer_allocate, nullptr, &readback_memory_),
             "vkAllocateMemory(readback)");
    check_vk(vkBindBufferMemory(device_, readback_buffer_, readback_memory_, 0),
             "vkBindBufferMemory");
}

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

void OffscreenRenderer::write_png() {
    void* mapped = nullptr;
    check_vk(vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");

    const std::size_t byte_count = static_cast<std::size_t>(config_.width) * config_.height * 4;
    pixels_.resize(byte_count);
    std::memcpy(pixels_.data(), mapped, byte_count);
    vkUnmapMemory(device_, readback_memory_);

    const int ok = stbi_write_png(config_.output_path.c_str(), static_cast<int>(config_.width),
                                  static_cast<int>(config_.height), 4, pixels_.data(),
                                  static_cast<int>(config_.width * 4));
    if (ok == 0) {
        throw std::runtime_error("failed to write PNG: " + config_.output_path);
    }

    std::cout << "wrote " << config_.output_path << " (" << config_.width << "x" << config_.height
              << ")\n";
}

void OffscreenRenderer::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
            command_buffer_ = VK_NULL_HANDLE;
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (image_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, image_view_, nullptr);
            image_view_ = VK_NULL_HANDLE;
        }
        if (image_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_, image_, nullptr);
            image_ = VK_NULL_HANDLE;
        }
        if (image_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, image_memory_, nullptr);
            image_memory_ = VK_NULL_HANDLE;
        }
        if (readback_buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, readback_buffer_, nullptr);
            readback_buffer_ = VK_NULL_HANDLE;
        }
        if (readback_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, readback_memory_, nullptr);
            readback_memory_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

} // namespace tessera::rendering::vulkan
