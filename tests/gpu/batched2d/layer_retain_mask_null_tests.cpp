// SPDX-License-Identifier: MIT
//
// Retained layers (issue #617, docs/design/gpu/offscreen-layers-retention.md) meet `mask-composite`
// on the null backend: a mask with a non-add (subtract) part is part of what a layer retains. The
// first frame renders the mask and its part once and records the layer; hits composite only and run
// neither; a dirty neighbour beside a hit renders its mask and part with balanced passes; and a
// kept layer takes no mask. Recycling hazards stay clean.

#include "layer_hazard_device.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/mask_composite.hpp"
#include "tessera/rendering/redraw_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::RedrawPlan;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::gpu::MaskCompositeOp;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;
constexpr std::uint64_t kKey = 0xabcdef12u;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_retain_mask_null: " << message << "\n";
    }
    return condition;
}

struct Scene {
    layer_hazard::Log log;
    layer_hazard::Device device{log};
    std::unique_ptr<rhi::RenderTarget> target;
    std::unique_ptr<rhi::Texture> white = device.create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> blur = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> composite = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> mask = make_mask();
    std::unique_ptr<rhi::Pipeline> mask_out = make_blend(rhi::BlendMode::SourceOut);
    std::unique_ptr<Batched2DRenderer> renderer;
    tessera::rendering::null::NullCommandStats last;
    std::uint32_t width = kWidth;
    std::uint32_t height = kHeight;

    explicit Scene(Batched2DConfig config = {}) {
        target = device.create_render_target(rhi::RenderTargetDesc{{kWidth, kHeight}});
        renderer = std::make_unique<Batched2DRenderer>(
            device, *pipeline, *white, config,
            GlassPipelines{blur.get(), composite.get(), mask.get(), mask_out.get()});
        log.ignore(target.get());
    }
    std::unique_ptr<rhi::Pipeline> make_blend(rhi::BlendMode blend) {
        rhi::PipelineDesc desc;
        desc.blend = blend;
        return device.create_pipeline(desc);
    }
    std::unique_ptr<rhi::Pipeline> make_mask() {
        rhi::PipelineDesc desc;
        desc.blend = rhi::BlendMode::DestinationIn;
        return device.create_pipeline(desc);
    }
    [[nodiscard]] rhi::RenderTargetDesc desc() const {
        rhi::RenderTargetDesc out;
        out.extent = {width, height};
        return out;
    }
    // One frame; `paint` records the scene between begin() and end(). `damage` (optional)
    // clips the frame to those regions through begin_damaged().
    template<typename Paint>
    void frame(Paint&& paint, const std::vector<rhi::Scissor>* damage = nullptr) {
        auto commands = device.create_command_list();
        if (damage != nullptr) {
            RedrawPlan plan;
            plan.scissors = *damage;
            renderer->begin_damaged(*commands, *target, desc(), plan);
        } else {
            renderer->begin(*commands, *target, desc());
        }
        paint(*renderer);
        renderer->end();
        commands->submit();
        last = static_cast<layer_hazard::CommandList&>(*commands).stats();
    }
};

rhi::Rect card(int index) {
    return rhi::Rect{static_cast<float>(8 + index * 40), 8.0f, 36.0f, 36.0f};
}

// A masked card whose mask holds a subtract part (mask-composite) under its bottom layer.
bool part_masked_card(Batched2DRenderer& renderer, int index, bool clean = true) {
    const rhi::Rect box = card(index);
    const bool hit = renderer.retain_layer(static_cast<std::uint64_t>(index + 1), kKey, clean);
    renderer.push_layer(box, 1.0f);
    if (!hit) {
        renderer.push_rect(box, rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
        if (renderer.begin_layer_mask()) {
            const rhi::Color black{0.0f, 0.0f, 0.0f, 1.0f};
            renderer.push_rect({box.x, box.y, box.width * 0.5f, box.height}, black);
            if (renderer.begin_mask_layer(MaskCompositeOp::Subtract)) {
                renderer.push_rect({box.x, box.y, box.width * 0.25f, box.height}, black);
                renderer.end_mask_layer();
            }
            renderer.end_layer_mask();
        }
    }
    renderer.pop_layer();
    return hit;
}

bool clean_stream(const Scene& scene, const char* what) {
    return expect(scene.log.lost_writes == 0, what) &&
           expect(scene.log.sampled_pass_conflicts == 0, what) &&
           expect(scene.log.unsampled_targets() == 0, what) &&
           expect(scene.last.begin_render_passes == scene.last.end_render_passes, what);
}

// A mask with a non-add part is part of what a layer retains: the first frame renders the part and
// the mask once, hits composite only, and a dirty neighbour next to a hit still balances its part.
bool mask_parts_and_retention() {
    Scene scene;
    const auto cards = [](Batched2DRenderer& renderer, bool second_clean) {
        const bool a = part_masked_card(renderer, 0);
        const bool b = part_masked_card(renderer, 1, second_clean);
        return (a ? 1u : 0u) + (b ? 1u : 0u);
    };
    std::uint32_t hits = 99;
    scene.frame([&](Batched2DRenderer& renderer) { hits = cards(renderer, true); });
    const auto first = scene.last;
    bool ok =
        expect(hits == 0 && scene.renderer->retained_layer_stats().records == 2,
               "parts: both layers record") &&
        expect(scene.renderer->layer_masks_applied() == 2 &&
                   scene.renderer->layer_masks_unsupported() == 0,
               "parts: each mask, part included, ran once") &&
        expect(first.begin_render_passes == first.end_render_passes, "parts: passes balanced") &&
        clean_stream(scene, "parts: recording frame");

    scene.frame([&](Batched2DRenderer& renderer) { hits = cards(renderer, true); });
    ok =
        expect(hits == 2, "parts: both hit") &&
        expect(scene.renderer->layer_masks_applied() == 0 && scene.renderer->layers_isolated() == 0,
               "parts: a hit runs no mask and no part") &&
        expect(scene.last.draws == 2 && scene.last.begin_render_passes == 1 + 2,
               "parts: only the composites are drawn") &&
        clean_stream(scene, "parts: hit frame") && ok;

    // Card 1 turns dirty while card 0 stays kept: card 1 renders mask and part, card 0 composites.
    scene.frame([&](Batched2DRenderer& renderer) { hits = cards(renderer, false); });
    const auto& mixed = scene.last;
    ok =
        expect(hits == 1, "parts: the clean neighbour still hits") &&
        expect(scene.renderer->layers_isolated() == 1 && scene.renderer->layer_masks_applied() == 1,
               "parts: the dirty one renders its mask") &&
        expect(mixed.begin_render_passes == mixed.end_render_passes,
               "parts: passes balanced across a hit and a part") &&
        expect(scene.renderer->retained_layer_stats().entries == 1,
               "parts: the dirty owner dropped its entry") &&
        clean_stream(scene, "parts: mixed frame") && ok;

    // The seam refuses a mask on a layer that was kept: nothing may be drawn between a hit's pair.
    scene.frame([&](Batched2DRenderer& renderer) {
        const bool hit = renderer.retain_layer(1, kKey, true);
        renderer.push_layer(card(0), 1.0f);
        ok = expect(hit && !renderer.begin_layer_mask(), "parts: a kept layer takes no mask") && ok;
        renderer.pop_layer();
    });
    return clean_stream(scene, "parts: refused mask on a kept layer") && ok;
}

} // namespace

int main() {
    try {
        if (!mask_parts_and_retention()) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_retain_mask_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_retain_mask_null: ok\n";
    return 0;
}
