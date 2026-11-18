// SPDX-License-Identifier: MIT
//
// Issue #480: the number of live textures is not capped by one descriptor pool. Holding more
// textures than a pool has sets must succeed (the device grows its pool list), and releasing them
// must leave the device usable for another round (freed sets are reused).

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

// Two and a bit pools of 1024 sets, all alive at once.
constexpr int kLiveTextures = 2200;

bool hold_and_release(tessera::rendering::rhi::Device& device, const char* round) {
    std::vector<std::unique_ptr<tessera::rendering::rhi::Texture>> live;
    live.reserve(kLiveTextures);
    try {
        for (int i = 0; i < kLiveTextures; ++i) {
            tessera::rendering::rhi::TextureDesc desc;
            desc.extent = {1, 1};
            live.push_back(device.create_texture(desc));
            if (live.back() == nullptr) {
                std::cerr << "vulkan_descriptor_pool_growth: null texture at " << i << " (" << round
                          << ")\n";
                return false;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "vulkan_descriptor_pool_growth: " << round << " failed after " << live.size()
                  << " live textures: " << error.what() << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = false;

    std::unique_ptr<tessera::rendering::rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_descriptor_pool_growth: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }
    if (!hold_and_release(*device, "first round")) {
        return 1;
    }
    // The first round's sets went back to their pools; a second identical round must not need more.
    return hold_and_release(*device, "second round") ? 0 : 2;
}
