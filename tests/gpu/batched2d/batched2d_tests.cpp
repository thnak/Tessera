// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <iostream>

int main() {
    using namespace tessera::rendering;

    auto device = null::create_null_device(rhi::DeviceDesc{});
    auto target = device->create_render_target(rhi::RenderTargetDesc{{256, 256}});
    auto white = device->create_texture(rhi::TextureDesc{{1, 1}});
    auto checker = device->create_texture(rhi::TextureDesc{{2, 2}});
    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto commands = device->create_command_list();

    gpu::Batched2DRenderer renderer(*device, *pipeline, *white);
    renderer.begin(*commands, *target, rhi::RenderTargetDesc{{256, 256}});

    renderer.set_transform(rhi::Mat3::translation(8, 8));
    renderer.push_rect({0, 0, 32, 32}, rhi::rgba8(200, 30, 30));
    renderer.push_rounded_rect({40, 0, 48, 32}, rhi::rgba8(30, 200, 30), 8);
    renderer.set_transform(rhi::Mat3::identity());

    renderer.push_texture({0, 64, 64, 64}, *checker, {0, 0, 1, 1}, rhi::rgba8(255, 255, 255));

    renderer.push_scissor({16, 16, 64, 64});
    renderer.push_rect({0, 0, 128, 128}, rhi::rgba8(30, 30, 200));
    renderer.pop_scissor();

    renderer.push_rect({160, 160, 32, 32}, rhi::rgba8(200, 200, 30));
    renderer.end();
    commands->submit();

    auto* null_commands = dynamic_cast<null::NullCommandList*>(commands.get());
    if (null_commands == nullptr) {
        std::cerr << "unexpected command list type\n";
        return 1;
    }

    const null::NullCommandStats& stats = null_commands->stats();
    if (stats.begin_render_passes != 1 || stats.end_render_passes != 1) {
        std::cerr << "render pass stats mismatch\n";
        return 2;
    }
    if (stats.draws != 4 || stats.vertices != 30) {
        std::cerr << "draw stats mismatch: draws=" << stats.draws << " vertices=" << stats.vertices
                  << "\n";
        return 3;
    }
    if (renderer.draw_calls() != 4 || renderer.vertex_count() != 30) {
        std::cerr << "renderer stats mismatch\n";
        return 4;
    }
    if (!null_commands->submitted()) {
        std::cerr << "command list was not submitted\n";
        return 5;
    }

    std::cout << "batched2d: ok\n";
    return 0;
}
