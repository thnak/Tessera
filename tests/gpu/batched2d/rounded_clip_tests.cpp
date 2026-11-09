// SPDX-License-Identifier: MIT
//
// Real-Vulkan rounded-clip readback: a child quad painted under a rounded clip must leave the
// pixels outside the parent's rounded corner untouched (background), while the clipped interior is
// painted. Deterministic: UNORM target, AA disabled (golden config), exact byte comparison.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;

constexpr Extent2D kExtent{64, 64};
constexpr Color kBackground{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kRed = rgba8(255, 0, 0);

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "rounded_clip: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = kBackground;
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

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool is_color(const std::array<std::uint8_t, 4>& pixel, std::uint8_t r, std::uint8_t g,
              std::uint8_t b) {
    return pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == 255;
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
        std::cout << "rounded_clip: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    const RenderTargetDesc desc = target_desc();
    auto target = device->create_render_target(desc);
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    auto pipeline = device->create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    // AA off + golden mode: hard edges and exact bytes.
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});

    auto commands = device->create_command_list();
    renderer.begin(*commands, *target, desc);
    renderer.push_clip(tessera::rendering::gpu::clip::uniform_rect({8, 8, 48, 48}, 16.0f));
    renderer.push_rect({0, 0, 64, 64}, kRed);
    renderer.pop_clip();
    renderer.end();
    commands->submit();
    device->wait_idle();

    const std::vector<std::uint8_t> clipped = read_target(*device, *target);
    bool ok =
        expect(is_color(pixel_at(clipped, 32, 32), 255, 0, 0), "clipped interior is painted") &&
        expect(is_color(pixel_at(clipped, 10, 10), 0, 0, 0),
               "pixel outside the rounded corner stays background") &&
        expect(is_color(pixel_at(clipped, 53, 10), 0, 0, 0), "top-right corner is clipped too") &&
        expect(is_color(pixel_at(clipped, 10, 53), 0, 0, 0), "bottom-left corner is clipped too") &&
        expect(is_color(pixel_at(clipped, 2, 32), 0, 0, 0),
               "pixel outside the clip rect stays background") &&
        expect(is_color(pixel_at(clipped, 8, 32), 255, 0, 0), "clip rect edge keeps content");

    // Control: the same quad without the clip paints the corner pixels.
    auto control_commands = device->create_command_list();
    renderer.begin(*control_commands, *target, desc);
    renderer.push_rect({0, 0, 64, 64}, kRed);
    renderer.end();
    control_commands->submit();
    device->wait_idle();
    const std::vector<std::uint8_t> unclipped = read_target(*device, *target);
    ok = expect(is_color(pixel_at(unclipped, 10, 10), 255, 0, 0),
                "unclipped control paints the corner") &&
         ok;

    if (!ok) {
        std::cerr << "rounded_clip: failed\n";
        return 1;
    }
    std::cout << "rounded_clip: ok\n";
    return 0;
}
