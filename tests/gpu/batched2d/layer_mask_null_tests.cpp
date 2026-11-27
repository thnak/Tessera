// SPDX-License-Identifier: MIT
//
// Layer masks (issue #617, docs/design/gpu/offscreen-layers.md section 5.2) on the null backend,
// driven straight through Batched2DRenderer so the recorded command stream is what is asserted:
//  - a masked layer records frame -> layer pass -> mask pass (cleared, second pooled target) ->
//    mask composite (sampled bind, reopened layer pass, one draw) -> layer composite (sampled bind,
//    reopened frame pass, one draw);
//  - every fallback records nothing extra and reports false so the caller skips the mask draws: no
//    mask pipeline, a flat layer, a pool with no second target, a layer already masked, no layer;
//  - the mask pipeline is requested with `BlendMode::DestinationIn` by the production helper's
//    contract, which the null recording of the pipeline makes assertable.

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
        std::cerr << "layer_mask_null: " << message << "\n";
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
    std::unique_ptr<rhi::Pipeline> mask = make_mask();
    std::unique_ptr<rhi::CommandList> commands = device->create_command_list();

    std::unique_ptr<rhi::Pipeline> make_mask() {
        rhi::PipelineDesc desc;
        desc.blend = rhi::BlendMode::DestinationIn;
        return device->create_pipeline(desc);
    }
    const NullCommandStats& stats() const {
        return static_cast<const tessera::rendering::null::NullCommandList*>(commands.get())
            ->stats();
    }
    GlassPipelines pipelines(bool with_mask = true) const {
        return GlassPipelines{blur.get(), composite.get(), with_mask ? mask.get() : nullptr};
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
using Kind = NullCommandKind;

bool masked_layer_order() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               fixture.pipelines());
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_layer(kBox, 1.0f);
    renderer.push_rect({10.0f, 10.0f, 40.0f, 40.0f}, kRed); // the content
    if (!expect(renderer.begin_layer_mask(), "the mask opens")) {
        return false;
    }
    renderer.push_rect({10.0f, 10.0f, 20.0f, 40.0f}, kRed); // the mask image
    renderer.end_layer_mask();
    renderer.pop_layer();
    renderer.end();
    fixture.commands->submit();

    const NullCommandStats& stats = fixture.stats();
    bool ok = expect(renderer.layers_isolated() == 1 && renderer.layer_masks_applied() == 1 &&
                         renderer.layer_masks_unsupported() == 0,
                     "one isolated, masked layer") &&
              expect(stats.begin_render_passes == 5 && stats.end_render_passes == 5,
                     "frame, layer, mask, reopened layer and reopened frame pass") &&
              expect(stats.sampled_target_binds == 2,
                     "the mask and then the layer are each sampled once") &&
              expect(renderer.offscreen_target_count() == 2, "the layer and its mask target");
    std::vector<Kind> skeleton;
    for (const Kind kind : stats.command_order) {
        if (kind == Kind::BeginRenderPass || kind == Kind::EndRenderPass || kind == Kind::Draw ||
            kind == Kind::BindSampledTarget) {
            skeleton.push_back(kind);
        }
    }
    const std::vector<Kind> want{
        Kind::BeginRenderPass,   // frame
        Kind::EndRenderPass,     // frame closes at push_layer (nothing drawn before it)
        Kind::BeginRenderPass,   // layer
        Kind::Draw,              // the content
        Kind::EndRenderPass,     // layer closes at begin_layer_mask
        Kind::BeginRenderPass,   // mask
        Kind::Draw,              // the mask image
        Kind::EndRenderPass,     // mask closes at end_layer_mask
        Kind::BindSampledTarget, // the mask
        Kind::BeginRenderPass,   // the layer reopened (Load)
        Kind::Draw,              // mask composite (DestinationIn)
        Kind::EndRenderPass,     // layer closes at pop_layer
        Kind::BindSampledTarget, // the layer
        Kind::BeginRenderPass,   // the frame reopened
        Kind::Draw,              // layer composite
        Kind::EndRenderPass};
    return expect(skeleton == want, "pass and draw skeleton of one masked layer") && ok;
}

bool mask_pipeline_records_the_blend() {
    Fixture fixture;
    const auto* recorded =
        dynamic_cast<const tessera::rendering::null::NullPipeline*>(fixture.mask.get());
    return expect(recorded != nullptr && recorded->blend_mode() == rhi::BlendMode::DestinationIn,
                  "the mask pipeline carries DestinationIn");
}

// Runs a frame whose one layer cannot be masked; nothing extra is recorded and the caller is told.
bool fallback(const char* what, Batched2DRenderer& renderer, Fixture& fixture, bool open_layer) {
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    if (open_layer) {
        renderer.push_layer(kBox, 0.5f);
    }
    const bool began = renderer.begin_layer_mask();
    if (open_layer) {
        renderer.pop_layer();
    }
    renderer.end();
    fixture.commands->submit();
    return expect(!began, what) && expect(renderer.layer_masks_unsupported() == 1, what) &&
           expect(renderer.layer_masks_applied() == 0, what);
}

bool without_the_mask_pipeline() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               fixture.pipelines(false));
    bool ok = fallback("no mask pipeline reports false", renderer, fixture, true);
    // The layer itself still isolated: only the mask is missing.
    return expect(renderer.layers_isolated() == 1, "the layer stayed isolated") && ok &&
           expect(fixture.stats().begin_render_passes == 3, "no mask pass was opened");
}

bool without_a_layer() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               fixture.pipelines());
    return fallback("no layer open reports false", renderer, fixture, false) &&
           expect(fixture.stats().begin_render_passes == 1, "the stream is the single pass");
}

bool flat_layer() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {}, {});
    return fallback("a flat layer cannot be masked", renderer, fixture, true) &&
           expect(renderer.layers_flat() == 1, "the layer was flat");
}

bool pool_out_of_targets() {
    Fixture fixture;
    Batched2DConfig config;
    config.max_offscreen_targets = 1; // the layer takes it; the mask finds none
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, config,
                               fixture.pipelines());
    return fallback("a pool with no second target reports false", renderer, fixture, true) &&
           expect(renderer.layers_isolated() == 1, "the layer was isolated") &&
           expect(fixture.stats().begin_render_passes == 3, "no mask pass was opened");
}

bool already_masked() {
    Fixture fixture;
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, {},
                               fixture.pipelines());
    renderer.begin(*fixture.commands, *fixture.target, target_desc());
    renderer.push_layer(kBox, 1.0f);
    const bool first = renderer.begin_layer_mask();
    renderer.end_layer_mask();
    const bool second = renderer.begin_layer_mask();
    renderer.pop_layer();
    renderer.end();
    fixture.commands->submit();
    return expect(first, "the first mask opens") && expect(!second, "a layer takes one mask") &&
           expect(renderer.layer_masks_applied() == 1 && renderer.layer_masks_unsupported() == 1,
                  "one applied, one refused");
}

} // namespace

int main() {
    try {
        const bool ok = masked_layer_order() && mask_pipeline_records_the_blend() &&
                        without_the_mask_pipeline() && without_a_layer() && flat_layer() &&
                        pool_out_of_targets() && already_masked();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_mask_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_mask_null: ok\n";
    return 0;
}
