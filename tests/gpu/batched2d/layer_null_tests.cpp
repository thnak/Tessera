// SPDX-License-Identifier: MIT
//
// Isolated layers (issue #227, docs/design/gpu/offscreen-layers.md section 5.2) on the null
// backend, driven straight through Batched2DRenderer so the recorded command stream is what is
// asserted:
//  - one layer records frame draws -> end pass -> layer pass (cleared, one pooled target) -> end
//    pass -> sampled bind -> reopened frame pass -> composite draw -> the draws after the layer;
//  - layers nest (two pooled targets) and the pool is bounded: past it a layer paints flat and is
//    counted, recording no extra pass;
//  - a renderer without the composite pipeline, a layer with nothing visible and a tree with no
//    layer all record exactly the single-pass stream they did before this seam existed.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::null::NullCommandKind;
using tessera::rendering::null::NullCommandStats;

constexpr std::uint32_t kSize = 100;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_null: " << message << "\n";
    }
    return condition;
}

struct Fixture {
    std::unique_ptr<rhi::Device> device = tessera::rendering::null::create_null_device({});
    std::unique_ptr<rhi::RenderTarget> target =
        device->create_render_target(rhi::RenderTargetDesc{{kSize, kSize}});
    std::unique_ptr<rhi::Texture> white = device->create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> blur = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> composite = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::CommandList> commands = device->create_command_list();

    const NullCommandStats& stats() const {
        return static_cast<const tessera::rendering::null::NullCommandList*>(commands.get())
            ->stats();
    }
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

const rhi::Rect kBox{10.0f, 10.0f, 40.0f, 40.0f};
const rhi::Color kRed{1.0f, 0.0f, 0.0f, 1.0f};

GlassPipelines pipelines_of(const Fixture& fixture) {
    return GlassPipelines{fixture.blur.get(), fixture.composite.get()};
}

using Kind = NullCommandKind;

bool one_layer_order() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               pipelines_of(fixture));
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_rect({0.0f, 0.0f, 20.0f, 20.0f}, kRed); // before the layer
    renderer.push_layer(kBox, 0.5f);
    renderer.push_rect({20.0f, 20.0f, 20.0f, 20.0f}, kRed); // inside
    renderer.push_rect({25.0f, 25.0f, 20.0f, 20.0f}, kRed); // inside, overlapping
    renderer.pop_layer();
    renderer.push_rect({60.0f, 60.0f, 10.0f, 10.0f}, kRed); // after
    renderer.end();
    fixture.commands->submit();

    const NullCommandStats& stats = fixture.stats();
    const bool ok =
        expect(renderer.layers_isolated() == 1 && renderer.layers_flat() == 0,
               "the layer was isolated") &&
        expect(stats.begin_render_passes == 3 && stats.end_render_passes == 3,
               "frame, layer and the reopened frame pass") &&
        expect(stats.sampled_target_binds == 1, "the composite samples the layer once") &&
        expect(stats.sampled_targets.size() == 1 && stats.sampled_targets[0].slot == 0,
               "the layer is bound at slot 0");
    if (!ok) {
        return false;
    }
    if (!expect(stats.draws == 4, "pre-layer, layer, composite, post-layer draws")) {
        return false;
    }
    // The order check is about the structure, not the exact pipeline rebinds, so compare only the
    // pass and draw skeleton.
    std::vector<Kind> skeleton;
    for (const Kind kind : stats.command_order) {
        if (kind == Kind::BeginRenderPass || kind == Kind::EndRenderPass || kind == Kind::Draw ||
            kind == Kind::BindSampledTarget || kind == Kind::Submit) {
            skeleton.push_back(kind);
        }
    }
    const std::vector<Kind> want{Kind::BeginRenderPass,
                                 Kind::Draw,
                                 Kind::EndRenderPass,
                                 Kind::BeginRenderPass,
                                 Kind::Draw,
                                 Kind::EndRenderPass,
                                 Kind::BindSampledTarget,
                                 Kind::BeginRenderPass,
                                 Kind::Draw,
                                 Kind::Draw,
                                 Kind::EndRenderPass,
                                 Kind::Submit};
    return expect(skeleton == want, "pass and draw skeleton of one isolated layer");
}

bool nested_and_bounded() {
    Fixture fixture;
    Batched2DConfig config;
    config.max_offscreen_targets = 2;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, config,
                               pipelines_of(fixture));
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_layer(kBox, 0.5f);
    renderer.push_rect({12.0f, 12.0f, 10.0f, 10.0f}, kRed);
    renderer.push_layer(kBox, 0.5f); // nested: the second pooled target
    renderer.push_rect({14.0f, 14.0f, 10.0f, 10.0f}, kRed);
    renderer.push_layer(kBox, 0.5f); // the pool is out of targets: flat
    renderer.push_rect({16.0f, 16.0f, 10.0f, 10.0f}, kRed);
    renderer.pop_layer();
    renderer.pop_layer();
    renderer.pop_layer();
    renderer.end();
    fixture.commands->submit();
    const NullCommandStats& stats = fixture.stats();
    return expect(renderer.layers_isolated() == 2 && renderer.layers_flat() == 1,
                  "two isolated layers and one flat past the bound") &&
           expect(stats.sampled_target_binds == 2, "two composites") &&
           expect(stats.begin_render_passes == stats.end_render_passes,
                  "every opened pass is closed") &&
           expect(renderer.offscreen_target_count() == 2, "the pool stayed at its bound");
}

bool without_the_pipeline_is_flat() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {}, {});
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_layer(kBox, 0.5f);
    renderer.push_rect({20.0f, 20.0f, 20.0f, 20.0f}, kRed);
    renderer.pop_layer();
    renderer.end();
    fixture.commands->submit();
    const NullCommandStats& stats = fixture.stats();
    return expect(!renderer.layers_supported(), "no composite pipeline, no layers") &&
           expect(renderer.layers_isolated() == 0 && renderer.layers_flat() == 1,
                  "the layer painted flat") &&
           expect(stats.begin_render_passes == 1 && stats.sampled_target_binds == 0 &&
                      stats.draws == 1,
                  "the stream is the single-pass stream");
}

bool nothing_visible_is_flat() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               pipelines_of(fixture));
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_layer({500.0f, 500.0f, 10.0f, 10.0f}, 0.5f); // outside the frame
    renderer.push_rect({500.0f, 500.0f, 10.0f, 10.0f}, kRed);
    renderer.pop_layer();
    renderer.end();
    fixture.commands->submit();
    return expect(renderer.layers_isolated() == 0 && renderer.layers_flat() == 1,
                  "a layer outside the frame opens no target") &&
           expect(fixture.stats().begin_render_passes == 1, "no extra pass");
}

bool frame_without_layers_is_unchanged() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               pipelines_of(fixture));
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_rect({0.0f, 0.0f, 20.0f, 20.0f}, kRed);
    renderer.end();
    fixture.commands->submit();
    const NullCommandStats& stats = fixture.stats();
    return expect(stats.begin_render_passes == 1 && stats.end_render_passes == 1 &&
                      stats.sampled_target_binds == 0 && stats.draws == 1,
                  "a frame with no layer records one pass and one draw");
}

} // namespace

int main() {
    try {
        const bool ok = one_layer_order() && nested_and_bounded() &&
                        without_the_pipeline_is_flat() && nothing_visible_is_flat() &&
                        frame_without_layers_is_unchanged();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_null: ok\n";
    return 0;
}
