// SPDX-License-Identifier: MIT
//
// Helpers shared by the OffscreenRenderer translation units (offscreen_renderer.cpp: instance,
// device, target, teardown; offscreen_renderer_record.cpp: pipeline, commands, submit).

#pragma once

#include <tessera/rendering/vulkan/offscreen_renderer.hpp>

#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan::offscreen_detail {

inline constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult=" +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

} // namespace tessera::rendering::vulkan::offscreen_detail
