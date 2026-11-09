// SPDX-License-Identifier: MIT
//
// Texture uploads are queued without waiting. Destroying a texture while its upload may still be
// running must be deferred by the device (a destroyed image under an in-flight copy is a Vulkan
// validity error), and repeated writes to one texture (the atlas pattern) must stay ordered. The
// validation layer reports violations as "[vulkan] ..." lines, which fail this test in ctest.

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Texture;
using tessera::rendering::rhi::TextureDesc;

constexpr std::uint32_t kSize = 64;
constexpr std::size_t kBytes = static_cast<std::size_t>(kSize) * kSize * 4;
constexpr int kChurnIterations = 300;
constexpr int kAtlasWrites = 200;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_async_upload: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> pixels(std::uint8_t value) {
    return std::vector<std::uint8_t>(kBytes, value);
}

// Create, rewrite and destroy textures back to back, never waiting for the GPU.
bool check_create_write_destroy_churn(Device& device) {
    for (int i = 0; i < kChurnIterations; ++i) {
        const std::vector<std::uint8_t> initial = pixels(static_cast<std::uint8_t>(i));
        TextureDesc desc;
        desc.extent = {kSize, kSize};
        desc.pixels = initial.data();

        std::unique_ptr<Texture> texture = device.create_texture(desc);
        if (!expect(texture != nullptr, "create_texture returned a texture")) {
            return false;
        }
        const std::vector<std::uint8_t> update = pixels(static_cast<std::uint8_t>(255 - i % 256));
        texture->write(update.data(), update.size());
        texture.reset(); // the copies above may still be queued or running
    }
    return true;
}

// One long-lived texture rewritten many times, as a glyph atlas is.
bool check_atlas_rewrites(Device& device) {
    TextureDesc desc;
    desc.extent = {kSize, kSize};
    std::unique_ptr<Texture> texture = device.create_texture(desc);
    if (!expect(texture != nullptr, "atlas texture created")) {
        return false;
    }
    for (int i = 0; i < kAtlasWrites; ++i) {
        const std::vector<std::uint8_t> update = pixels(static_cast<std::uint8_t>(i));
        texture->write(update.data(), update.size());
    }
    return true;
}

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_async_upload: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    try {
        if (!check_create_write_destroy_churn(*device)) {
            return 1;
        }
        if (!check_atlas_rewrites(*device)) {
            return 2;
        }
        // Destroying the device with uploads and deferred destruction possibly still pending
        // must free everything without leaks (VMA asserts in debug builds).
        device->wait_idle();
    } catch (const std::exception& error) {
        std::cerr << "vulkan_async_upload: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    return 0;
}
