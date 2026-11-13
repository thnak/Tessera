// SPDX-License-Identifier: MIT

#include "parity_render.hpp"
#include "parity_scenes.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace tessera::tests::gpu::backends {
namespace {

using namespace tessera::rendering;

const Scene* find_scene(std::string_view name) {
    for (const Scene& scene : parity_scenes()) {
        if (scene.name == name) {
            return &scene;
        }
    }
    return nullptr;
}

} // namespace

RenderedScene render_scene(rhi::Device& device, rhi::Pipeline& pipeline, std::string_view scene) {
    const Scene* definition = find_scene(scene);
    if (definition == nullptr) {
        throw std::runtime_error("unknown parity scene");
    }

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kSceneWidth, kSceneHeight};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = parity_clear_color();
    target_desc.load_op = rhi::LoadOp::Clear;
    target_desc.store_op = rhi::StoreOp::Store;
    std::unique_ptr<rhi::RenderTarget> target = device.create_render_target(target_desc);
    if (target == nullptr) {
        throw std::runtime_error("create_render_target returned null");
    }

    std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    std::unique_ptr<rhi::Texture> white = device.create_texture(white_desc);
    if (white == nullptr) {
        throw std::runtime_error("create_texture returned null");
    }

    SceneTextures textures = create_scene_textures(device);
    // AA off + golden mode: hard edges and exact bytes, which is what makes a cross-backend
    // comparison about rasterization and not about coverage estimation.
    rendering::gpu::Batched2DRenderer renderer(device, pipeline, *white,
                                               rendering::gpu::Batched2DConfig{4096, false, true});

    std::unique_ptr<rhi::CommandList> commands = device.create_command_list();
    if (commands == nullptr) {
        throw std::runtime_error("create_command_list returned null");
    }
    renderer.begin(*commands, *target, target_desc);
    definition->draw(renderer, textures);
    renderer.end();
    commands->submit();

    RenderedScene rendered;
    rendered.width = kSceneWidth;
    rendered.height = kSceneHeight;
    rendered.draw_calls = renderer.draw_calls();
    rendered.vertices = renderer.vertex_count();

    const std::size_t bytes = static_cast<std::size_t>(kSceneWidth) * kSceneHeight * 4;
    std::unique_ptr<rhi::Buffer> readback =
        device.create_buffer({bytes, rhi::BufferUsage::Readback});
    if (readback == nullptr) {
        throw std::runtime_error("create_buffer returned null");
    }
    std::unique_ptr<rhi::CommandList> copy = device.create_command_list();
    if (copy == nullptr) {
        throw std::runtime_error("create_command_list returned null");
    }
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    rendered.rgba.resize(bytes);
    readback->read(rendered.rgba.data(), rendered.rgba.size());
    return rendered;
}

} // namespace tessera::tests::gpu::backends
