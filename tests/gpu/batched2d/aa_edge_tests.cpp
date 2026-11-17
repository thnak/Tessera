// SPDX-License-Identifier: MIT
//
// Screen-space AA edges and rounded-clip AA on a real Vulkan device
// (docs/design/display-aware-rendering.md S2 and S4):
//  - an SDF edge at the same fractional pixel position produces the same coverage whether the quad
//    is drawn at 1x, 2x or 0.5x (the edge width is device pixels, not local units);
//  - with AA on a rounded clip has intermediate coverage at its corner and a pixel-aligned clip
//    edge is still exactly opaque inside and untouched outside;
//  - golden mode keeps the clip hard (only 0 and 255 on the corner).
// Skips cleanly when no Vulkan device is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;

constexpr Extent2D kExtent{64, 64};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "aa_edge: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
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

// Red channel: white over black, so it equals the coverage the shader wrote.
int red_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    return pixels[(static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t)];
}

template<typename Draw>
std::vector<std::uint8_t> render(Device& device, RenderTarget& target, const RenderTargetDesc& desc,
                                 Batched2DRenderer& renderer, Draw&& draw) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, desc);
    draw(renderer);
    renderer.end();
    commands->submit();
    device.wait_idle();
    return read_target(device, target);
}

bool near(int a, int b, int tolerance) {
    return std::abs(a - b) <= tolerance;
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
        std::cout << "aa_edge: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    const RenderTargetDesc desc = target_desc();
    auto target = device->create_render_target(desc);
    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    Batched2DRenderer aa(*device, *pipeline, *white, Batched2DConfig{4096, true, false});
    Batched2DRenderer golden(*device, *pipeline, *white, Batched2DConfig{4096, false, true});

    const auto white_color = rgba8(255, 255, 255);
    bool ok = true;

    // The same device-space edge (left edge at x = 10.25, so pixel 10 is a quarter covered) drawn
    // at 1x, 2x and 0.5x. The shader's edge width is device pixels, so all three agree.
    const auto at_1x = render(*device, *target, desc, aa, [&](Batched2DRenderer& r) {
        r.push_rounded_rect({10.25f, 8.0f, 30.0f, 40.0f}, white_color, 2.0f);
    });
    const auto at_2x = render(*device, *target, desc, aa, [&](Batched2DRenderer& r) {
        r.set_transform(Mat3::scale(2.0f, 2.0f));
        r.push_rounded_rect({5.125f, 4.0f, 15.0f, 20.0f}, white_color, 1.0f);
        r.set_transform(Mat3::identity());
    });
    const auto at_half = render(*device, *target, desc, aa, [&](Batched2DRenderer& r) {
        r.set_transform(Mat3::scale(0.5f, 0.5f));
        r.push_rounded_rect({20.5f, 16.0f, 60.0f, 80.0f}, white_color, 4.0f);
        r.set_transform(Mat3::identity());
    });
    const int edge_1x = red_at(at_1x, 10, 28);
    const int edge_2x = red_at(at_2x, 10, 28);
    const int edge_half = red_at(at_half, 10, 28);
    ok = expect(edge_1x > 0 && edge_1x < 255, "the fractional edge pixel has partial coverage") &&
         ok;
    ok = expect(near(edge_1x, edge_2x, 3), "edge coverage at 2x matches 1x") && ok;
    ok = expect(near(edge_1x, edge_half, 3), "edge coverage at 0.5x matches 1x") && ok;
    // A half-pixel edge radius: pixel 10 centre is 0.25 px inside, so about 84% covered.
    ok = expect(near(edge_1x, 215, 4), "a quarter-covered pixel reads about 84%") && ok;

    // Rounded clip over a full-target fill. The clip is 6..58 with a 12 px corner radius.
    const auto clip = tessera::rendering::gpu::clip::make_rounded_rect({6.0f, 6.0f, 52.0f, 52.0f},
                                                                       12.0f, 12.0f, 12.0f, 12.0f);
    const auto clipped = [&](Batched2DRenderer& renderer) {
        return render(*device, *target, desc, renderer, [&](Batched2DRenderer& r) {
            r.push_clip(clip);
            r.push_rect({0.0f, 0.0f, 64.0f, 64.0f}, white_color);
            r.pop_clip();
        });
    };
    const auto aa_clip = clipped(aa);
    const auto golden_clip = clipped(golden);

    // The corner arc passes near (8, 8)..(10, 10); count intermediate values along the diagonal.
    int aa_partial = 0;
    int golden_partial = 0;
    for (std::uint32_t i = 6; i < 16; ++i) {
        const int a = red_at(aa_clip, i, i);
        const int g = red_at(golden_clip, i, i);
        aa_partial += (a > 0 && a < 255) ? 1 : 0;
        golden_partial += (g > 0 && g < 255) ? 1 : 0;
    }
    ok = expect(aa_partial > 0, "AA clip has partial coverage at its rounded corner") && ok;
    ok = expect(golden_partial == 0, "golden clip stays hard at its rounded corner") && ok;

    // Straight clip edges are pixel aligned: fully opaque inside, untouched outside, in both.
    for (const auto* pixels : {&aa_clip, &golden_clip}) {
        ok = expect(red_at(*pixels, 32, 6) == 255 && red_at(*pixels, 32, 5) == 0,
                    "aligned top clip edge is exact") &&
             ok;
        ok = expect(red_at(*pixels, 6, 32) == 255 && red_at(*pixels, 5, 32) == 0,
                    "aligned left clip edge is exact") &&
             ok;
        ok = expect(red_at(*pixels, 57, 32) == 255 && red_at(*pixels, 58, 32) == 0,
                    "aligned right clip edge is exact") &&
             ok;
        ok = expect(red_at(*pixels, 32, 32) == 255, "clip interior is opaque") && ok;
    }

    // Deterministic repeat.
    ok = expect(clipped(aa) == aa_clip, "repeated AA clip render is byte-identical") && ok;

    if (!ok) {
        std::cerr << "aa_edge: failed\n";
        return 1;
    }
    std::cout << "aa_edge: ok (edge 1x/2x/0.5x = " << edge_1x << "/" << edge_2x << "/" << edge_half
              << ", aa corner partial pixels = " << aa_partial << ")\n";
    return 0;
}
