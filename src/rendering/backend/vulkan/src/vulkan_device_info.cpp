// SPDX-License-Identifier: MIT
//
// Device provenance for goldens (ADR-009, docs/specs/golden-tests-v1.md section 1): a run that
// writes or verifies a pixel baseline has to know whether the pixels came from a software
// rasterizer. The class is decided from the physical device itself, not from `VK_ICD_FILENAMES`,
// because an environment variable says what was requested, not what answered.

#include "tessera/rendering/vulkan/vulkan_device_info.hpp"
#include "vulkan_internal.hpp"

#include <string>
#include <string_view>

namespace tessera::rendering::vulkan {
namespace {

// Vulkan's own enumerators, without including <vulkan/vulkan_core.h> from a public header.
constexpr std::uint32_t kDeviceTypeCpu = 4; // VK_PHYSICAL_DEVICE_TYPE_CPU
constexpr std::uint32_t kDriverIdMesaLlvmpipe = 13;
constexpr std::uint32_t kDriverIdGoogleSwiftShader = 10;

bool names_software(std::string_view driver_name) {
    // Mesa reports "llvmpipe" for lavapipe; the other names cover a driver packaged differently and
    // a run whose properties came from a wrapper.
    return driver_name.find("llvmpipe") != std::string_view::npos ||
           driver_name.find("lavapipe") != std::string_view::npos ||
           driver_name.find("SwiftShader") != std::string_view::npos ||
           driver_name.find("swiftshader") != std::string_view::npos;
}

VulkanDeviceInfo read_properties(const VulkanDevice& device) {
    VulkanDeviceInfo info;
    const VkPhysicalDevice physical = device.physical_device();
    if (physical == VK_NULL_HANDLE) {
        return info;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    info.device_name = properties.deviceName;
    info.device_type = static_cast<std::uint32_t>(properties.deviceType);
    info.api_version = properties.apiVersion;

    // VK_KHR_driver_properties is core in 1.2 and every device Tessera accepts is 1.3+, so the
    // query is available; it is still treated as optional because a driver may report nothing.
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    info.driver_name = driver.driverName;
    info.driver_id = static_cast<std::uint32_t>(driver.driverID);
    return info;
}

} // namespace

bool VulkanDeviceInfo::software() const noexcept {
    // Type first: a Mesa CPU device is authoritative even when its driver name is empty. The driver
    // id and the name are the fallbacks for a device that reports itself as something else.
    return device_type == kDeviceTypeCpu || driver_id == kDriverIdMesaLlvmpipe ||
           driver_id == kDriverIdGoogleSwiftShader || names_software(driver_name);
}

std::string VulkanDeviceInfo::describe() const {
    std::string text = device_name.empty() ? std::string("<unknown device>") : device_name;
    text += " [";
    text += software() ? "software" : "hardware";
    if (!driver_name.empty()) {
        text += ", driver ";
        text += driver_name;
    }
    text += ", deviceType " + std::to_string(device_type);
    text += ", driverID " + std::to_string(driver_id);
    text += ", API " + std::to_string(VK_VERSION_MAJOR(api_version)) + "." +
            std::to_string(VK_VERSION_MINOR(api_version)) + "." +
            std::to_string(VK_VERSION_PATCH(api_version));
    text += "]";
    return text;
}

std::optional<VulkanDeviceInfo> device_info(const rhi::Device& device) noexcept {
    if (device.backend() != rhi::Backend::Vulkan) {
        return std::nullopt;
    }
    // `backend()` is the type tag: every object of the Vulkan backend is a VulkanDevice (the
    // `vulkan_cast` contract), so the downcast is the same tag check the frame path uses.
    const auto* concrete = dynamic_cast<const VulkanDevice*>(&device);
    if (concrete == nullptr) {
        return std::nullopt;
    }
    return read_properties(*concrete);
}

} // namespace tessera::rendering::vulkan
