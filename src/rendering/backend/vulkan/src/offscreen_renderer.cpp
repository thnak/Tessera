// SPDX-License-Identifier: MIT

#include "offscreen_renderer_internal.hpp"
#include <tessera/rendering/vulkan/offscreen_renderer.hpp>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <stb_image_write.h>

namespace {

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

using offscreen_detail::check_vk;
using offscreen_detail::kColorFormat;

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
