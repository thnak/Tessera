// SPDX-License-Identifier: MIT
//
// GPU memory accounting (issue #13): Device::memory_statistics() splits live bytes into buffers and
// images, and returns to the baseline when the resources are released. The null backend reports
// zero; the Vulkan backend (when a device exists) must show the per-resource costs the budgets in
// docs/design/performance.md quote: a 1024x1024 R8 glyph atlas page is 1 MiB, a 4x multisampled
// 1280x720 RGBA target is the resolve image plus four times that.

#include "tessera/rendering/null/null_device.hpp"
#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

namespace {

using namespace tessera::rendering;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "rhi_memory: " << message << "\n";
    }
    return condition;
}

#if defined(TESSERA_TEST_HAS_VULKAN)
bool check_vulkan(rhi::Device& device) {
    const auto base = device.memory_statistics();
    bool ok = expect(base.buffer_bytes == 0 && base.image_bytes == 0,
                     "a fresh device holds no tracked resources");

    constexpr std::uint32_t kAtlas = 1024;
    rhi::TextureDesc atlas_desc;
    atlas_desc.extent = {kAtlas, kAtlas};
    atlas_desc.format = rhi::TextureFormat::R8Unorm;
    // No initial pixels: an upload would also hold a staging buffer until the copy finishes.
    auto atlas = device.create_texture(atlas_desc);
    auto vertices = device.create_buffer({64 * 1024, rhi::BufferUsage::Vertex});

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {1280, 720};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    auto target = device.create_render_target(target_desc);

    const auto during = device.memory_statistics();
    constexpr std::uint64_t kTargetBytes = 1280ULL * 720 * 4;
    ok = expect(during.buffer_bytes == 64 * 1024, "buffer bytes are the requested size") && ok;
    ok = expect(during.image_bytes == kAtlas * kAtlas + kTargetBytes,
                "image bytes are the atlas page plus the render target") &&
         ok;
    ok = expect(during.allocation_bytes >= during.buffer_bytes + during.image_bytes,
                "the allocator holds at least the tracked bytes") &&
         ok;

    if (device.supports_sample_count(rhi::TextureFormat::R8G8B8A8Unorm, 4)) {
        target_desc.sample_count = 4;
        auto msaa = device.create_render_target(target_desc);
        const auto with_msaa = device.memory_statistics();
        ok = expect(with_msaa.image_bytes == during.image_bytes + kTargetBytes * 5,
                    "a 4x target counts the resolve image plus four samples") &&
             ok;
    }

    atlas.reset();
    vertices.reset();
    target.reset();
    const auto after = device.memory_statistics();
    ok = expect(after.buffer_bytes == 0 && after.image_bytes == 0,
                "releasing everything returns both counters to zero") &&
         ok;
    return ok;
}
#endif

} // namespace

int main() {
    auto null_device = null::create_null_device(rhi::DeviceDesc{});
    const auto stats = null_device->memory_statistics();
    if (!expect(stats.block_bytes == 0 && stats.buffer_bytes == 0 && stats.image_bytes == 0,
                "the null backend reports zero")) {
        return 1;
    }

#if defined(TESSERA_TEST_HAS_VULKAN)
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    std::unique_ptr<rhi::Device> device;
    try {
        device = vulkan::create_vulkan_device(desc);
    } catch (const std::exception& error) {
        std::cout << "rhi_memory: vulkan skipped (" << error.what() << ")\n";
        return 0;
    }
    if (!check_vulkan(*device)) {
        return 2;
    }
#endif
    std::cout << "rhi_memory: ok\n";
    return 0;
}
