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
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_equivalence: " << message << "\n";
    }
    return condition;
}

// Phase 0 draws the left card at 8,8 in blue; phase 1 moves it to 14,14 and recolors it. The
// right card never changes, so a damage region around the left card must reproduce phase 1
// exactly while preserving the untouched right card from phase 0.
void draw_scene(Batched2DRenderer& renderer, int phase) {
    renderer.push_rect({0, 0, 64, 64}, rgba8(11, 18, 32));
    if (phase == 0) {
        renderer.push_rect({8, 8, 20, 20}, rgba8(56, 189, 248));
    } else {
        renderer.push_rect({14, 14, 20, 20}, rgba8(251, 146, 60));
    }
    renderer.push_rect({36, 36, 20, 20}, rgba8(34, 197, 94));
}

void render_full(Batched2DRenderer& renderer, Device& device, RenderTarget& target,
                 const RenderTargetDesc& desc, int phase) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, desc);
    draw_scene(renderer, phase);
    renderer.end();
    commands->submit();
    device.wait_idle();
}

void render_damaged(Batched2DRenderer& renderer, Device& device, RenderTarget& target,
                    const RenderTargetDesc& desc, int phase,
                    const std::vector<tessera::rendering::rhi::Scissor>& regions) {
    auto commands = device.create_command_list();
    renderer.begin_damaged(*commands, target, desc, tessera::rendering::RedrawPlan{false, regions});
    draw_scene(renderer, phase);
    renderer.end();
    commands->submit();
    device.wait_idle();
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes = 64 * 64 * 4;
    auto readback = device.create_buffer({bytes, tessera::rendering::rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::size_t mismatched(const std::vector<std::uint8_t>& actual,
                       const std::vector<std::uint8_t>& expected) {
    return tessera::rendering::golden::compare_rgba({64, 64}, actual, {64, 64}, expected, 0)
        .mismatched;
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

    RenderTargetDesc clear_desc;
    clear_desc.extent = {64, 64};
    clear_desc.clear_color = rgba8(11, 18, 32);
    clear_desc.load_op = rhi::LoadOp::Clear;
    RenderTargetDesc load_desc = clear_desc;
    load_desc.load_op = rhi::LoadOp::Load;

    // Static frame: partial redraw of the same content must equal a full redraw.
    auto static_target = device->create_render_target(clear_desc);
    render_full(renderer, *device, *static_target, clear_desc, 0);
    render_damaged(renderer, *device, *static_target, load_desc, 0, {rhi::Scissor{6, 6, 26, 26}});
    const std::vector<std::uint8_t> static_partial = read_target(*device, *static_target);
    auto static_reference = device->create_render_target(clear_desc);
    render_full(renderer, *device, *static_reference, clear_desc, 0);
    const std::vector<std::uint8_t> static_full = read_target(*device, *static_reference);
    if (!expect(mismatched(static_partial, static_full) == 0,
                "static partial redraw equals full redraw")) {
        return 1;
    }

    // Dynamic frame: the left card moved and recolored; the damage region covers old and new
    // bounds, so the partial result must match a full phase-1 frame bit-exactly.
    auto dynamic_target = device->create_render_target(clear_desc);
    render_full(renderer, *device, *dynamic_target, clear_desc, 0);
    render_damaged(renderer, *device, *dynamic_target, load_desc, 1, {rhi::Scissor{6, 6, 30, 30}});
    const std::vector<std::uint8_t> dynamic_partial = read_target(*device, *dynamic_target);
    auto dynamic_reference = device->create_render_target(clear_desc);
    render_full(renderer, *device, *dynamic_reference, clear_desc, 1);
    const std::vector<std::uint8_t> dynamic_full = read_target(*device, *dynamic_reference);
    if (!expect(mismatched(dynamic_partial, dynamic_full) == 0,
                "dynamic partial redraw equals full redraw")) {
        return 2;
    }

    // Negative control: too small a damage region must leave stale pixels and fail the equality,
    // proving the dynamic check above is not vacuous.
    auto stale_target = device->create_render_target(clear_desc);
    render_full(renderer, *device, *stale_target, clear_desc, 0);
    render_damaged(renderer, *device, *stale_target, load_desc, 1, {rhi::Scissor{10, 10, 10, 10}});
    const std::vector<std::uint8_t> stale = read_target(*device, *stale_target);
    if (!expect(mismatched(stale, dynamic_full) > 0, "undersized damage leaves stale pixels")) {
        return 3;
    }

    std::cout << "damage_equivalence: ok\n";
    return 0;
}
