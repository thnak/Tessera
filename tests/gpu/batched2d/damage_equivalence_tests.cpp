// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_equivalence: " << message << "\n";
    }
    return condition;
}

void draw_scene(Batched2DRenderer& renderer) {
    renderer.push_rect({0, 0, 64, 64}, rgba8(11, 18, 32));
    renderer.push_rect({8, 8, 20, 20}, rgba8(56, 189, 248));
    renderer.push_rect({36, 36, 20, 20}, rgba8(34, 197, 94));
}

std::vector<std::uint8_t> read_target(tessera::rendering::rhi::Device& device,
                                      tessera::rendering::rhi::CommandList& commands,
                                      RenderTarget& target) {
    const std::size_t bytes = 64 * 64 * 4;
    auto readback = device.create_buffer({bytes, tessera::rendering::rhi::BufferUsage::Readback});
    commands.copy_target_to_buffer(target, *readback);
    commands.submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

} // namespace

int main() {
    using namespace tessera::rendering;

    auto device = vulkan::create_vulkan_device(rhi::DeviceDesc{rhi::Backend::Vulkan, false, -1});
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    auto pipeline = device->create_pipeline(vulkan::batched2d_pipeline_desc());
    Batched2DRenderer renderer(*device, *pipeline, *white, gpu::Batched2DConfig{4096, true, true});

    RenderTargetDesc full_desc;
    full_desc.extent = {64, 64};
    full_desc.clear_color = rgba8(11, 18, 32);
    full_desc.load_op = rhi::LoadOp::Clear;
    auto target = device->create_render_target(full_desc);

    // Frame 1 establishes the full image in the offscreen target.
    auto first_commands = device->create_command_list();
    renderer.begin(*first_commands, *target, full_desc);
    draw_scene(renderer);
    renderer.end();
    first_commands->submit();
    device->wait_idle();

    // Frame 2 redraws only the left damage rect while preserving the rest of the target.
    RenderTargetDesc partial_desc = full_desc;
    partial_desc.load_op = rhi::LoadOp::Load;
    const RedrawPlan plan{false, {rhi::Scissor{6, 6, 26, 26}}};
    auto partial_commands = device->create_command_list();
    renderer.begin_damaged(*partial_commands, *target, partial_desc, plan);
    draw_scene(renderer);
    renderer.end();
    partial_commands->submit();
    device->wait_idle();

    auto read_commands = device->create_command_list();
    const std::vector<std::uint8_t> partial = read_target(*device, *read_commands, *target);

    // Reference: a fresh full frame with the same scene.
    RenderTargetDesc reference_desc = full_desc;
    auto reference = device->create_render_target(reference_desc);
    auto reference_commands = device->create_command_list();
    renderer.begin(*reference_commands, *reference, reference_desc);
    draw_scene(renderer);
    renderer.end();
    const std::vector<std::uint8_t> expected =
        read_target(*device, *reference_commands, *reference);

    const auto diff = golden::compare_rgba({64, 64}, partial, {64, 64}, expected, 0);
    if (!expect(diff.size_mismatch == false, "same size") ||
        !expect(diff.mismatched == 0, "partial redraw equals full redraw") ||
        !expect(diff.max_channel_delta == 0, "pixels are bit-exact")) {
        std::cout << "damage_equivalence: mismatched=" << diff.mismatched
                  << " max_delta=" << diff.max_channel_delta << "\n";
        return 1;
    }

    std::cout << "damage_equivalence: ok\n";
    return 0;
}
