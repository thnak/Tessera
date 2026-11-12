// SPDX-License-Identifier: MIT

#include "vulkan_device_selection.hpp"
#include "vulkan_internal.hpp"

#include <cstring>
#include <iostream>
#include <vector>

#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif

namespace tessera::rendering::vulkan {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        // Cheap enough to count every message, and only reached when validation is on, so a GPU
        // test can assert a run was clean instead of scraping stderr.
        record_validation_message();
        std::cerr << "[vulkan] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

// `layer` selects extensions provided by an instance layer (e.g. VK_EXT_validation_features comes
// from the validation layer, not the loader); nullptr queries the loader and drivers.
bool has_instance_extension(const char* name, const char* layer = nullptr) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(layer, &count, properties.data()) != VK_SUCCESS) {
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

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
bool has_device_extension(VkPhysicalDevice device, const char* name) {
    std::uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()) !=
        VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}
#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE

} // namespace

void VulkanDevice::create_instance() {
    check_vk(volkInitialize(), "volkInitialize");

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Tessera";
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
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME) &&
        has_instance_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        surface_extensions_enabled_ = true;
    }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    if (has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME) &&
        has_instance_extension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        surface_extensions_enabled_ = true;
    }
#endif

    std::vector<const char*> layers;
    const bool validation_available = has_layer("VK_LAYER_KHRONOS_validation");
    const bool validation_enabled = desc_.validation && validation_available;
    if (desc_.validation && !validation_available) {
        std::cerr << "[vulkan] warning: validation layer not available\n";
    }
    // Synchronization validation catches missing semaphores/barriers that core validation cannot
    // (for example presenting an image before the GPU finished rendering it).
    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    const VkValidationFeatureEnableEXT enabled_features[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT validation_features{};
    validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = enabled_features;
    bool sync_validation = false;
    if (validation_enabled) {
        layers.push_back(kValidationLayer);
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        if (has_instance_extension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, kValidationLayer)) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            sync_validation = true;
        }
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pNext = sync_validation ? &validation_features : nullptr;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    if (portability_available) {
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    check_vk(vkCreateInstance(&create_info, nullptr, &instance_), "vkCreateInstance");
    volkLoadInstance(instance_);

    if (validation_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = debug_callback;
        check_vk(
            vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &debug_messenger_),
            "vkCreateDebugUtilsMessengerEXT");
        mark_validation_active();
    }
}

void VulkanDevice::pick_device() {
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
    if (desc_.device_index >= 0) {
        if (static_cast<std::uint32_t>(desc_.device_index) >= device_count) {
            throw std::runtime_error("requested Vulkan device index is out of range");
        }
        chosen = desc_.device_index;
    } else {
        std::vector<DeviceCandidate> candidates(device_count);
        for (std::uint32_t i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[i], &properties);
            candidates[i].api_version = properties.apiVersion;
            switch (properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                candidates[i].kind = DeviceKind::Discrete;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                candidates[i].kind = DeviceKind::Integrated;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                candidates[i].kind = DeviceKind::Virtual;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                candidates[i].kind = DeviceKind::Cpu;
                break;
            default:
                break;
            }
        }
        // With no 1.3 device this falls back to index 0 so the check below reports it clearly.
        chosen = static_cast<int>(choose_device(candidates).value_or(0));
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

#if defined(VK_USE_PLATFORM_XLIB_KHR)
    // Xlib presentation support is queried per queue family with a Display* and VisualID. The
    // device keeps no connection, so probe with a short-lived one; a missing display leaves
    // presentation unsupported while the offscreen device stays usable.
    Display* probe_display = XOpenDisplay(nullptr);
    VisualID probe_visual = 0;
    if (probe_display != nullptr) {
        probe_visual =
            XVisualIDFromVisual(DefaultVisual(probe_display, DefaultScreen(probe_display)));
    }
#endif

    bool found_graphics = false;
    for (std::uint32_t i = 0; i < queue_count; ++i) {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (!found_graphics) {
            queue_family_ = i;
            found_graphics = true;
        }
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        // Prefer a graphics family that can also present to a Win32 surface. Presentation support
        // is queryable before a surface exists, unlike vkGetPhysicalDeviceSurfaceSupportKHR.
        if (vkGetPhysicalDeviceWin32PresentationSupportKHR != nullptr &&
            vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_device_, i) == VK_TRUE) {
            queue_family_ = i;
            win32_present_supported_ = true;
            break;
        }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
        if (probe_display != nullptr && vkGetPhysicalDeviceXlibPresentationSupportKHR != nullptr &&
            vkGetPhysicalDeviceXlibPresentationSupportKHR(physical_device_, i, probe_display,
                                                          probe_visual) == VK_TRUE) {
            queue_family_ = i;
            xlib_present_supported_ = true;
            break;
        }
#endif
    }
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    if (probe_display != nullptr) {
        XCloseDisplay(probe_display);
    }
#endif
    if (!found_graphics) {
        throw std::runtime_error("no graphics queue family found");
    }
}

void VulkanDevice::create_device() {
    VkPhysicalDeviceVulkan13Features supported_13{};
    supported_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features supported_12{};
    supported_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported_13.pNext = &supported_12;

    VkPhysicalDeviceFeatures2 supported_2{};
    supported_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported_2.pNext = &supported_13;
    vkGetPhysicalDeviceFeatures2(physical_device_, &supported_2);

    if (supported_13.dynamicRendering == VK_FALSE) {
        throw std::runtime_error("device does not support Vulkan 1.3 dynamic rendering");
    }
    // Frame pacing on the swapchain path is a timeline semaphore (docs/design/gpu/frames.md section
    // 5.1). Vulkan 1.3 mandates the feature, so a device without it is a broken driver or a lie
    // about its version: fail loudly instead of silently falling back to fence-per-slot pacing.
    if (supported_12.timelineSemaphore == VK_FALSE) {
        throw std::runtime_error("device does not support Vulkan 1.2 timeline semaphores");
    }
    VkPhysicalDeviceVulkan13Features enabled_13{};
    enabled_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled_13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled_12{};
    enabled_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled_12.timelineSemaphore = VK_TRUE;
    enabled_13.pNext = &enabled_12;

    VkPhysicalDeviceFeatures2 enabled_2{};
    enabled_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled_2.pNext = &enabled_13;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    std::vector<const char*> device_extensions;
#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    if (has_device_extension(physical_device_, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        swapchain_extension_enabled_ = true;
    }
#endif

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = &enabled_2;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames =
        device_extensions.empty() ? nullptr : device_extensions.data();

    check_vk(vkCreateDevice(physical_device_, &create_info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

void VulkanDevice::create_allocator() {
    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo create_info{};
    create_info.instance = instance_;
    create_info.physicalDevice = physical_device_;
    create_info.device = device_;
    create_info.vulkanApiVersion = VK_API_VERSION_1_3;
    check_vk(vmaImportVulkanFunctionsFromVolk(&create_info, &functions),
             "vmaImportVulkanFunctionsFromVolk");
    create_info.pVulkanFunctions = &functions;
    check_vk(vmaCreateAllocator(&create_info, &allocator_), "vmaCreateAllocator");
}

void VulkanDevice::create_descriptor_layout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    check_vk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_),
             "vkCreateDescriptorSetLayout");
}

void VulkanDevice::create_descriptor_pool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1024;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1024;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_),
             "vkCreateDescriptorPool");
}

void VulkanDevice::destroy() {
    if (device_ != VK_NULL_HANDLE && status_ == rhi::DeviceStatus::Ready) {
        (void)vkDeviceWaitIdle(device_);
        // Frees staging buffers and runs deferred destruction while the allocator still exists.
        drain_uploads();
        destroy_command_resources();
        destroy_device_objects();
    } else {
        // Lost device: child handles died with it, so only bookkeeping is dropped. A simulated
        // loss keeps the old device valid, so its wrappers can still release handles normally.
        if (simulated_loss_ && device_ != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device_);
            drain_uploads();
            destroy_command_resources();
            destroy_device_objects();
        } else {
            pending_uploads_.clear();
            retired_.clear();
            free_commands_.clear();
            inflight_commands_.clear();
            allocator_ = nullptr;
            if (device_ != VK_NULL_HANDLE) {
                vkDestroyDevice(device_, nullptr);
                device_ = VK_NULL_HANDLE;
            }
            physical_device_ = VK_NULL_HANDLE;
            queue_ = VK_NULL_HANDLE;
        }
    }
    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

} // namespace tessera::rendering::vulkan
