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

    // Rounded-clip split rule: a batch covers one (texture, scissor, folded clip uniform) tuple, so
    // push/pop splits only when the folded uniform set (or scissor) changes.
    auto clip_target = device->create_render_target(rhi::RenderTargetDesc{{64, 64}});
    auto clip_commands = device->create_command_list();
    gpu::Batched2DRenderer clip_renderer(*device, *pipeline, *white);
    clip_renderer.begin(*clip_commands, *clip_target, rhi::RenderTargetDesc{{64, 64}});
    const auto clip = gpu::clip::uniform_rect({0, 0, 64, 64}, 8.0f);
    clip_renderer.push_rect({0, 0, 8, 8}, rhi::rgba8(255, 0, 0)); // batch 0: no clip
    clip_renderer.push_clip(clip);
    clip_renderer.push_rect({8, 8, 8, 8}, rhi::rgba8(0, 255, 0)); // batch 1
    clip_renderer.push_rect({16, 8, 8, 8}, rhi::rgba8(0, 255, 0));
    clip_renderer.pop_clip();
    clip_renderer.push_rect({24, 8, 8, 8}, rhi::rgba8(0, 0, 255)); // batch 2: clip removed
    clip_renderer.push_clip(clip);
    clip_renderer.push_rect({32, 8, 8, 8}, rhi::rgba8(0, 0, 255)); // batch 3
    clip_renderer.pop_clip();
    clip_renderer.push_rect({40, 8, 8, 8}, rhi::rgba8(255, 255, 0)); // batch 4
    // A push/pop pair with no draw between must not add a draw.
    clip_renderer.push_clip(clip);
    clip_renderer.pop_clip();
    clip_renderer.end();
    clip_commands->submit();

    auto* clip_null = dynamic_cast<null::NullCommandList*>(clip_commands.get());
    if (clip_null == nullptr) {
        std::cerr << "unexpected clip command list type\n";
        return 6;
    }
    const null::NullCommandStats& clip_stats = clip_null->stats();
    if (clip_stats.draws != 5 || clip_stats.vertices != 36) {
        std::cerr << "clip split rule mismatch: draws=" << clip_stats.draws
                  << " vertices=" << clip_stats.vertices << "\n";
        return 7;
    }
    if (clip_renderer.draw_calls() != 5) {
        std::cerr << "clip renderer draw count mismatch\n";
        return 8;
    }

    // Deeper-than-uniform nesting folds conservatively into the innermost shader slot and still
    // emits a single batch for one quad.
    auto fold_target = device->create_render_target(rhi::RenderTargetDesc{{64, 64}});
    auto fold_commands = device->create_command_list();
    gpu::Batched2DRenderer fold_renderer(*device, *pipeline, *white);
    fold_renderer.begin(*fold_commands, *fold_target, rhi::RenderTargetDesc{{64, 64}});
    for (float radius : {2.0f, 4.0f, 6.0f, 8.0f, 10.0f}) {
        fold_renderer.push_clip(gpu::clip::uniform_rect({0, 0, 64, 64}, radius));
    }
    fold_renderer.push_rect({8, 8, 16, 16}, rhi::rgba8(0, 255, 255));
    for (int pop = 0; pop < 5; ++pop) {
        fold_renderer.pop_clip();
    }
    fold_renderer.end();
    fold_commands->submit();
    auto* fold_null = dynamic_cast<null::NullCommandList*>(fold_commands.get());
    const null::NullCommandStats& fold_stats = fold_null->stats();
    if (fold_stats.draws != 1 || fold_stats.vertices != 6) {
        std::cerr << "folded clip nesting mismatch: draws=" << fold_stats.draws
                  << " vertices=" << fold_stats.vertices << "\n";
        return 9;
    }

    std::cout << "batched2d: ok\n";
    return 0;
}
