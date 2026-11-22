// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace tessera::rendering::vulkan {

struct OffscreenConfig {
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    std::string output_path = "offscreen.png";
    bool enable_validation = false;
    int device_index = -1; // -1 = pick automatically (prefer discrete GPU)
};

// Phase 0 proof: Vulkan 1.3 dynamic rendering into an offscreen image,
// read back to host memory, written as a PNG. No window, no swapchain.
class OffscreenRenderer {
public:
    explicit OffscreenRenderer(OffscreenConfig config);
    ~OffscreenRenderer();

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    void render_to_png();

private:
    void create_instance();
    void pick_device();
    void create_device();
    void create_target();
    void create_pipeline();
    void create_command_objects();
    void record_and_submit();
    void write_png();
    void destroy();

    std::uint32_t find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags required) const;
    VkShaderModule make_shader(const std::uint32_t* code, std::size_t byte_count) const;

    OffscreenConfig config_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;

    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView image_view_ = VK_NULL_HANDLE;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;

    VkBuffer readback_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;

    std::vector<std::uint8_t> pixels_;
};

} // namespace tessera::rendering::vulkan
