// SPDX-License-Identifier: MIT
//
// MSAA 4x render-target parity on a real Vulkan device. A 4x target renders into a multisample
// image and resolves into its 1-sample image at the end of the pass:
//  - a scene of axis-aligned solid/SDF quads with AA off resolves byte-identically to 1-sample
//    (the resolve introduces no quality/colour change for geometry aligned to samples);
//  - a rotated quad's edges differ (MSAA edge coverage) while interior pixels match;
//  - repeated 4x renders are byte-identical (deterministic);
//  - a renderer/pipeline sample-count mismatch is rejected instead of silently rendering wrong.
// Skips cleanly when the device cannot do 4x for the format (the value is reported).

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;

constexpr Extent2D kExtent{64, 64};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "msaa: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc(std::uint32_t sample_count) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    desc.sample_count = sample_count;
    return desc;
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

void draw_scene(Batched2DRenderer& renderer, bool rotated) {
    if (rotated) {
        // A rotated quad has diagonal edges that MSAA covers with multiple samples.
        renderer.set_transform(tessera::rendering::rhi::Mat3::translation(32.0f, 32.0f) *
                               tessera::rendering::rhi::Mat3::rotation(0.4f));
        renderer.push_rect({-20.0f, -20.0f, 40.0f, 40.0f}, rgba8(220, 40, 40));
        renderer.set_transform(tessera::rendering::rhi::Mat3::identity());
        return;
    }
    // Axis-aligned solid + SDF rounded rect (AA off): sample-aligned geometry resolves identically.
    renderer.push_rect({4, 4, 24, 24}, rgba8(30, 120, 220));
    renderer.push_rounded_rect({34, 4, 24, 24}, rgba8(40, 200, 90), 8.0f);
}

std::vector<std::uint8_t> render(Device& device, RenderTarget& target, const RenderTargetDesc& desc,
                                 Batched2DRenderer& renderer, bool rotated) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, desc);
    draw_scene(renderer, rotated);
    renderer.end();
    commands->submit();
    device.wait_idle();
    return read_target(device, target);
}

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "msaa: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    if (!device->supports_sample_count(TextureFormat::R8G8B8A8Unorm, 4)) {
        std::cout << "msaa: skipped (R8G8B8A8Unorm sample_count=4 unsupported on this device)\n";
        return 0;
    }

    const RenderTargetDesc desc1 = target_desc(1);
    const RenderTargetDesc desc4 = target_desc(4);
    auto target1 = device->create_render_target(desc1);
    auto target4 = device->create_render_target(desc4);
    bool ok = expect(target1->sample_count() == 1 && target4->sample_count() == 4,
                     "render targets report their sample counts");

    auto pipeline1 = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1));
    auto pipeline4 = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 4));
    ok = expect(pipeline1->sample_count() == 1 && pipeline4->sample_count() == 4,
                "pipelines carry their sample counts") &&
         ok;

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    Batched2DRenderer renderer1(*device, *pipeline1, *white, Batched2DConfig{4096, false, true, 1});
    Batched2DRenderer renderer4(*device, *pipeline4, *white, Batched2DConfig{4096, false, true, 4});

    // A mismatch (1x renderer/pipeline on a 4x target) must be rejected, not silently rendered.
    bool rejected = false;
    try {
        auto commands = device->create_command_list();
        renderer1.begin(*commands, *target4, desc4);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ok = expect(rejected, "renderer rejects a sample-count mismatch") && ok;

    // Sample-aligned geometry: resolved 4x is byte-identical to 1x.
    const std::vector<std::uint8_t> aligned1 = render(*device, *target1, desc1, renderer1, false);
    const std::vector<std::uint8_t> aligned4 = render(*device, *target4, desc4, renderer4, false);
    ok = expect(aligned1 == aligned4, "aligned SDF/solid scene resolves byte-identically") && ok;

    // Rotated geometry: edge pixels differ (MSAA coverage), interior pixels match.
    const std::vector<std::uint8_t> rotated1 = render(*device, *target1, desc1, renderer1, true);
    const std::vector<std::uint8_t> rotated4 = render(*device, *target4, desc4, renderer4, true);
    std::size_t differing_pixels = 0;
    for (std::size_t i = 0; i + 3 < rotated1.size(); i += 4) {
        if (rotated1[i] != rotated4[i] || rotated1[i + 1] != rotated4[i + 1] ||
            rotated1[i + 2] != rotated4[i + 2] || rotated1[i + 3] != rotated4[i + 3]) {
            ++differing_pixels;
        }
    }
    ok = expect(differing_pixels > 0, "rotated edges differ between 1x and 4x") && ok;
    ok = expect(pixel_at(rotated1, 32, 32) == pixel_at(rotated4, 32, 32),
                "rotated-quad interior matches") &&
         ok;

    // Deterministic repeat: the same 4x scene renders byte-identically twice.
    const std::vector<std::uint8_t> rotated4_repeat =
        render(*device, *target4, desc4, renderer4, true);
    ok = expect(rotated4 == rotated4_repeat, "repeated 4x render is byte-identical") && ok;

    if (!ok) {
        std::cerr << "msaa: failed\n";
        return 1;
    }
    std::cout << "msaa: ok (rotated edge pixels differing=" << differing_pixels << ")\n";
    return 0;
}
