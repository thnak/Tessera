// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace tessera::rendering::vulkan {

// What device a run actually rendered on. Goldens are pinned to software rendering (ADR-009), so a
// run that writes or verifies a baseline has to be able to say which device produced the pixels
// instead of trusting an environment variable (docs/specs/golden-tests-v1.md section 1).
struct VulkanDeviceInfo {
    std::string device_name; // VkPhysicalDeviceProperties::deviceName
    std::string
        driver_name; // VkPhysicalDeviceDriverProperties::driverName when the driver reports it
    // VkPhysicalDeviceProperties::deviceType as its Vulkan enum value (4 = CPU), kept numeric so
    // this header stays includable without the Vulkan headers.
    std::uint32_t device_type = 0;
    std::uint32_t driver_id = 0; // VkPhysicalDeviceDriverProperties::driverID; 0 = unknown
    std::uint32_t api_version = 0;

    // True for a CPU device (`device_type == VK_PHYSICAL_DEVICE_TYPE_CPU`) or a driver that names
    // itself as software (llvmpipe/lavapipe/SwiftShader). Both tests are kept: a Mesa CPU device
    // reports type CPU, and a driver packaged another way still reports its name.
    [[nodiscard]] bool software() const noexcept;

    // One line for a failure message: device, driver and API version, so a reader of a refused
    // baseline knows exactly what produced the pixels.
    [[nodiscard]] std::string describe() const;
};

// Queries the device behind `device`. nullopt when it is not a Vulkan device (a null or WebGPU
// device has no Vulkan properties to report); an implementation that cannot read the driver
// properties still returns the device line, with `driver_name` empty.
[[nodiscard]] std::optional<VulkanDeviceInfo> device_info(const rhi::Device& device) noexcept;

} // namespace tessera::rendering::vulkan
