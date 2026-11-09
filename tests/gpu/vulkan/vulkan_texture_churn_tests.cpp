// SPDX-License-Identifier: MIT
//
// Regression: each texture owns a descriptor set from a fixed-size pool. Destroying a texture must
// return its set, otherwise create/destroy churn exhausts the pool after maxSets textures.

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <exception>
#include <iostream>
#include <memory>

namespace {

// Larger than the descriptor pool's maxSets (1024) so a leak is guaranteed to surface.
constexpr int kChurnIterations = 1100;

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = false;

    std::unique_ptr<tessera::rendering::rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_texture_churn: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    try {
        for (int i = 0; i < kChurnIterations; ++i) {
            tessera::rendering::rhi::TextureDesc desc;
            desc.extent = {1, 1};
            const auto texture = device->create_texture(desc);
            if (texture == nullptr) {
                std::cerr << "vulkan_texture_churn: create_texture returned null at " << i << "\n";
                return 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "vulkan_texture_churn: descriptor pool exhausted under churn: " << error.what()
                  << "\n";
        return 2;
    }
    return 0;
}
