// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel test for the isolated-layer seam (#242): draw, break the render pass, copy a
// region into a pooled offscreen target, resume and draw again.
//
// Asserts: pixels drawn before the break survive the Load resume; the draw after the break paints
// over them; the copy holds the source pixels at the requested offset; and a copied region can be
// placed back into the frame target. Deterministic UNORM target, exact byte comparison; the test
// skips cleanly when no Vulkan device is available.

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
using tessera::rendering::rhi::Offset2D;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;

constexpr Extent2D kExtent{64, 64};
constexpr Color kBlue = rgba8(0, 0, 255);
constexpr Color kRed = rgba8(255, 0, 0);
constexpr Color kGreen = rgba8(0, 255, 0);

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "renderer_seam_gpu: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = kBlue;
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
        std::cout << "renderer_seam_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
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

    // AA off + golden mode: hard edges and exact bytes.
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Batched2DRenderer renderer(*device, *pipeline, *white, config);

    // --- Draw, break, copy, resume, draw again ------------------------------------------------
    auto commands = device->create_command_list();
    renderer.begin(*commands, *target, desc);
    renderer.push_rect({0, 0, 64, 64}, kBlue);
    renderer.flush();
    renderer.push_rect({0, 0, 64, 16}, kRed); // red band along the top
    renderer.break_pass();
    auto* copy_target = renderer.acquire_offscreen_target(kExtent);
    if (!expect(copy_target != nullptr, "the offscreen pool hands out a target")) {
        return 1;
    }
    commands->copy_target_region(*target, Scissor{0, 0, 64, 16}, *copy_target, Offset2D{0, 0});
    renderer.resume_pass();
    renderer.push_rect({0, 0, 16, 16}, kGreen); // paints over part of the copied band
    renderer.end();
    commands->submit();
    device->wait_idle();

    const std::vector<std::uint8_t> frame = read_target(*device, *target);
    bool ok = expect(is_color(pixel_at(frame, 8, 8), 0, 255, 0),
                     "the draw after the break paints over the band") &&
              expect(is_color(pixel_at(frame, 32, 8), 255, 0, 0),
                     "content drawn before the break survives the Load resume") &&
              expect(is_color(pixel_at(frame, 32, 32), 0, 0, 255),
                     "pixels outside the resumed drawing keep the frame content");

    const std::vector<std::uint8_t> copied = read_target(*device, *copy_target);
    ok = expect(is_color(pixel_at(copied, 32, 8), 255, 0, 0),
                "the copied region holds the source pixels at the destination offset") &&
         expect(is_color(pixel_at(copied, 4, 4), 255, 0, 0),
                "the copy starts at the destination origin") &&
         ok;

    // --- Copy a region back into the frame target ---------------------------------------------
    auto back_commands = device->create_command_list();
    renderer.begin(*back_commands, *target, desc);
    renderer.push_rect({0, 0, 64, 64}, kBlue);
    renderer.break_pass();
    back_commands->copy_target_region(*copy_target, Scissor{0, 0, 64, 16}, *target,
                                      Offset2D{0, 32});
    renderer.resume_pass();
    renderer.end();
    back_commands->submit();
    device->wait_idle();

    const std::vector<std::uint8_t> composed = read_target(*device, *target);
    ok = expect(is_color(pixel_at(composed, 32, 40), 255, 0, 0),
                "a copied region can be placed back into the frame target") &&
         expect(is_color(pixel_at(composed, 32, 8), 0, 0, 255),
                "the rest of the frame target keeps the new frame's content") &&
         ok;

    if (!ok) {
        std::cerr << "renderer_seam_gpu: failed\n";
        return 1;
    }
    std::cout << "renderer_seam_gpu: ok\n";
    return 0;
}
