// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel test for the texture-array path (RHI 1.7, issue #480 part 2).
//
// The batched renderer ends a batch whenever the sampled texture changes, so images and glyph pages
// interleaved with text cost one draw call per switch. With descriptor indexing the same quads
// share one draw: the textures are bound as one array and each vertex carries its slot.
//
// What this pins, on a device that reports the capability:
//  - the draw-call count collapses (one draw for a scene that alternates two textures),
//  - the pixels are byte-identical to the one-set-per-texture path, which is the whole contract:
//    the array only changes how the same texels are addressed,
//  - a device without the capability keeps the old path (the test skips cleanly there).
//
// Deterministic UNORM target, exact bytes; skips cleanly when no Vulkan device is available.

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

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;

constexpr rhi::Extent2D kExtent{64, 64};
constexpr std::uint8_t kBackdrop = 24;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "texture_array_gpu: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = rhi::rgba8(kBackdrop, kBackdrop, kBackdrop);
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    return desc;
}

// A page of one solid colour, so a wrong slot is visible as a wrong colour rather than as a subtle
// sampling difference.
std::unique_ptr<rhi::Texture> solid_page(rhi::Device& device, std::uint8_t r, std::uint8_t g,
                                         std::uint8_t b) {
    std::vector<std::uint8_t> texels;
    for (int index = 0; index < 4 * 4; ++index) {
        texels.insert(texels.end(), {r, g, b, 255});
    }
    rhi::TextureDesc desc;
    desc.extent = {4, 4};
    desc.pixels = texels.data();
    desc.row_pitch = 4 * 4;
    return device.create_texture(desc);
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

// The scene: alternating quads over two textures, which is the interleaved image/text shape. Every
// quad is 8x8 at a distinct position, so a slot mix-up changes specific pixels.
void paint_scene(Batched2DRenderer& renderer, rhi::Texture& first, rhi::Texture& second) {
    const rhi::Rect uv{0, 0, 1, 1};
    const rhi::Color white = rhi::rgba8(255, 255, 255);
    for (std::uint32_t index = 0; index < 8; ++index) {
        rhi::Texture& texture = index % 2U == 0U ? first : second;
        renderer.push_texture({static_cast<float>(index) * 8.0f, 0.0f, 8.0f, 8.0f}, texture, uv,
                              white);
    }
}

struct Outcome {
    std::uint32_t draw_calls = 0;
    std::vector<std::uint8_t> pixels;
};

Outcome render(rhi::Device& device, rhi::RenderTarget& target, rhi::Pipeline& pipeline,
               rhi::Texture& white, rhi::Texture& first, rhi::Texture& second,
               rhi::Pipeline* array_pipeline) {
    auto commands = device.create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Batched2DRenderer renderer(device, pipeline, white, config);
    if (array_pipeline != nullptr) {
        renderer.set_texture_array_pipeline(array_pipeline);
    }
    const rhi::RenderTargetDesc desc = target_desc();
    renderer.begin(*commands, target, desc);
    paint_scene(renderer, first, second);
    renderer.end();
    commands->submit();
    device.wait_idle();

    Outcome outcome;
    outcome.draw_calls = renderer.draw_calls();
    outcome.pixels = read_target(device, target);
    return outcome;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "texture_array_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (device->texture_array_capacity() == 0) {
        std::cout << "texture_array_gpu: skipped (device reports no descriptor indexing)\n";
        return 0;
    }

    auto target = device->create_render_target(target_desc());
    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    auto array_pipeline =
        device->create_pipeline(tessera::rendering::vulkan::batched2d_array_pipeline_desc());

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    auto first = solid_page(*device, 255, 0, 0);
    auto second = solid_page(*device, 0, 255, 0);

    const Outcome plain = render(*device, *target, *pipeline, *white, *first, *second, nullptr);
    const Outcome array =
        render(*device, *target, *pipeline, *white, *first, *second, array_pipeline.get());

    bool ok =
        expect(plain.draw_calls == 8, "the one-set path records one draw per texture switch") &&
        expect(array.draw_calls == 1, "the array path records one draw for the whole scene") &&
        expect(plain.pixels == array.pixels,
               "the array path is byte-identical to the one-set path");

    // The scene must actually differ from the backdrop, or the identity check above is vacuous.
    std::size_t painted = 0;
    for (std::size_t index = 0; index + 3 < array.pixels.size(); index += 4) {
        if (array.pixels[index] != kBackdrop || array.pixels[index + 1] != kBackdrop ||
            array.pixels[index + 2] != kBackdrop) {
            ++painted;
        }
    }
    ok = expect(painted > 0, "the scene painted pixels") && ok;

    if (!ok) {
        std::cerr << "texture_array_gpu: failed\n";
        return 1;
    }
    std::cout << "texture_array_gpu: ok (draws " << plain.draw_calls << " -> " << array.draw_calls
              << ", painted=" << painted << ")\n";
    return 0;
}
