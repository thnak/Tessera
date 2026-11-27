// SPDX-License-Identifier: MIT
//
// The offscreen layer pool under real layer traffic (issue #617,
// docs/design/gpu/offscreen-layers.md section 5e), on the null backend, driven through
// Batched2DRenderer:
//  - N sibling masked layers (N = 1, 2, 6, 12) all isolate and all mask, on two pooled targets: the
//    layer and its mask target are recycled the moment they are dead, so peak live targets, not the
//    number of layers, bound the pool (a pool of four used to mask only the first two);
//  - the recorded stream never clears a target whose content nobody sampled (an early release would
//    hand a layer out while a later pass still needs it), checked by layer_hazard_device.hpp, which
//    must itself catch such a stream;
//  - peak live targets follow the documented table: 2 for a masked or blurred layer, 3 for a
//    drop-shadow, depth + 1 for nested masked layers;
//  - a cap that is still exceeded keeps the graceful fallback and counts it, per frame and in
//  total;
//  - a byte cap refuses a target the same way, and idle targets are trimmed.

#include "layer_hazard_device.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;
constexpr std::uint64_t kTargetBytes = static_cast<std::uint64_t>(kWidth) * kHeight * 4;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_pool_null: " << message << "\n";
    }
    return condition;
}

struct Scene {
    layer_hazard::Log log;
    layer_hazard::Device device{log};
    std::unique_ptr<rhi::RenderTarget> target =
        device.create_render_target(rhi::RenderTargetDesc{{kWidth, kHeight}});
    std::unique_ptr<rhi::Texture> white = device.create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> blur = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> composite = device.create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> mask = make_mask();
    std::unique_ptr<Batched2DRenderer> renderer;

    explicit Scene(Batched2DConfig config = {}) {
        renderer = std::make_unique<Batched2DRenderer>(
            device, *pipeline, *white, config,
            GlassPipelines{blur.get(), composite.get(), mask.get()});
        log.ignore(target.get());
    }
    std::unique_ptr<rhi::Pipeline> make_mask() {
        rhi::PipelineDesc desc;
        desc.blend = rhi::BlendMode::DestinationIn;
        return device.create_pipeline(desc);
    }
    static rhi::RenderTargetDesc desc() {
        rhi::RenderTargetDesc desc;
        desc.extent = {kWidth, kHeight};
        return desc;
    }
    // Runs one frame; `paint` records the scene between begin() and end().
    template<typename Paint> void frame(Paint&& paint) {
        auto commands = device.create_command_list();
        renderer->begin(*commands, *target, desc());
        paint(*renderer);
        renderer->end();
        commands->submit();
        const auto& stats = static_cast<layer_hazard::CommandList&>(*commands).stats();
        passes_begun = stats.begin_render_passes;
        passes_ended = stats.end_render_passes;
        sampled_binds = stats.sampled_target_binds;
        draws = stats.draws;
    }
    std::uint32_t passes_begun = 0;
    std::uint32_t passes_ended = 0;
    std::uint32_t sampled_binds = 0;
    std::uint32_t draws = 0;
};

rhi::Rect card(int index) {
    return rhi::Rect{static_cast<float>(8 + index * 40), 8.0f, 36.0f, 36.0f};
}

// One masked card: the layer's content, then a mask over its left half.
void masked_card(Batched2DRenderer& renderer, int index, const LayerFilterChain& filter = {}) {
    const rhi::Rect box = card(index);
    renderer.push_layer(box, 1.0f, filter);
    renderer.push_rect(box, rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
    if (renderer.begin_layer_mask()) {
        renderer.push_rect({box.x, box.y, box.width * 0.5f, box.height},
                           rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
        renderer.end_layer_mask();
    }
    renderer.pop_layer();
}

LayerFilterChain blur_chain() {
    LayerFilterChain chain;
    chain.count = 1;
    chain.ops[0].kind = LayerFilterOp::Kind::Blur;
    chain.ops[0].sigma = 2.0f;
    return chain;
}

LayerFilterChain shadow_chain() {
    LayerFilterChain chain;
    chain.count = 1;
    chain.ops[0].kind = LayerFilterOp::Kind::DropShadow;
    chain.ops[0].sigma = 2.0f;
    chain.ops[0].offset_x = 3.0f;
    chain.ops[0].offset_y = 3.0f;
    return chain;
}

bool clean(const Scene& scene, const char* what) {
    return expect(scene.log.lost_writes == 0, what) &&
           expect(scene.log.sampled_pass_conflicts == 0, what) &&
           expect(scene.log.unsampled_targets() == 0, what) &&
           expect(scene.passes_begun == scene.passes_ended, what);
}

// N masked siblings: every one isolates and masks, on the layer + mask pair of targets.
bool sibling_masked_layers(int count) {
    Scene scene;
    scene.frame([&](Batched2DRenderer& renderer) {
        for (int index = 0; index < count; ++index) {
            masked_card(renderer, index);
        }
    });
    const Batched2DRenderer& renderer = *scene.renderer;
    const auto stats = renderer.offscreen_stats();
    const auto n = static_cast<std::uint32_t>(count);
    return expect(renderer.layers_isolated() == n && renderer.layers_flat() == 0,
                  "every sibling layer isolates") &&
           expect(renderer.layer_masks_applied() == n && renderer.layer_masks_unsupported() == 0,
                  "every sibling layer is masked") &&
           expect(stats.exhausted == 0 && stats.exhausted_total == 0, "no request was refused") &&
           expect(stats.peak_live == 2, "peak live targets: the layer and its mask") &&
           expect(stats.targets == 2 && stats.bytes == 2 * kTargetBytes,
                  "two frame-size targets, whatever the number of layers") &&
           // frame + (layer, mask, layer reopened, frame reopened) per card
           expect(scene.passes_begun == 1 + 4 * n, "pass count follows the layer count") &&
           expect(scene.sampled_binds == 2 * n, "each mask and each layer is sampled once") &&
           expect(scene.log.used.size() == 2, "only two distinct targets were drawn into") &&
           clean(scene, "siblings: recycling hazards");
}

// The validator is only worth having if it fails a stream that loses a write.
bool hazard_device_catches_early_release() {
    Scene scene;
    // Drive the command list directly: clear, release before anything sampled it, acquire again,
    // clear again.
    auto commands = scene.device.create_command_list();
    scene.renderer->begin(*commands, *scene.target, Scene::desc());
    rhi::RenderTarget* first = scene.renderer->acquire_offscreen_target({64, 64});
    scene.renderer->break_pass();
    rhi::RenderTargetDesc clear;
    clear.extent = {64, 64};
    commands->begin_render_pass(*first, clear);
    commands->end_render_pass();
    scene.renderer->release_offscreen_target(first);
    rhi::RenderTarget* second = scene.renderer->acquire_offscreen_target({64, 64});
    commands->begin_render_pass(*second, clear);
    commands->end_render_pass();
    scene.renderer->end();
    commands->submit();
    return expect(second == first, "a released target is handed out again in the same frame") &&
           expect(scene.log.lost_writes == 1, "clearing an unsampled target is reported");
}

// Peak live targets for one card of each kind, and the same for six siblings.
bool peak_table() {
    struct Row {
        const char* what;
        LayerFilterChain filter;
        bool masked;
        std::uint32_t peak;
    };
    const Row rows[] = {
        {"plain layer", {}, false, 1},
        {"masked layer", {}, true, 2},
        {"blur", blur_chain(), false, 2},
        {"blur then mask", blur_chain(), true, 2},
        {"drop-shadow", shadow_chain(), false, 3},
        {"drop-shadow then mask", shadow_chain(), true, 3},
    };
    bool ok = true;
    for (const Row& row : rows) {
        Scene scene;
        scene.frame([&](Batched2DRenderer& renderer) {
            for (int index = 0; index < 6; ++index) {
                if (row.masked) {
                    masked_card(renderer, index, row.filter);
                } else {
                    renderer.push_layer(card(index), 1.0f, row.filter);
                    renderer.push_rect(card(index), rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
                    renderer.pop_layer();
                }
            }
        });
        const auto stats = scene.renderer->offscreen_stats();
        const bool shown = expect(stats.peak_live == row.peak, row.what) &&
                           expect(stats.targets == row.peak, row.what) &&
                           expect(stats.exhausted == 0, row.what) &&
                           expect(scene.renderer->layers_isolated() == 6, row.what) &&
                           expect(scene.renderer->layer_filters_unsupported() == 0, row.what) &&
                           clean(scene, row.what);
        ok = shown && ok;
    }
    return ok;
}

// Glass backdrops borrow their two targets for one node only: six cards, two targets.
bool glass_cards() {
    Scene scene;
    std::uint32_t drawn = 0;
    scene.frame([&](Batched2DRenderer& renderer) {
        renderer.push_rect({0.0f, 0.0f, 400.0f, 100.0f}, rhi::Color{0.2f, 0.4f, 0.8f, 1.0f});
        for (int index = 0; index < 6; ++index) {
            tessera::rendering::gpu::GlassBackdropDraw draw;
            draw.clip.rect = card(index);
            draw.sigma = 4.0f;
            drawn += renderer.draw_backdrop(draw) ? 1u : 0u;
        }
    });
    const auto stats = scene.renderer->offscreen_stats();
    return expect(drawn == 6, "every glass card composites") &&
           expect(stats.peak_live == 2 && stats.targets == 2 && stats.exhausted == 0,
                  "on the one source/blur pair of targets") &&
           clean(scene, "glass: recycling hazards");
}

// Nested masked layers hold one target per open layer plus the innermost mask.
void nest(Batched2DRenderer& renderer, int depth) {
    const rhi::Rect box{8.0f, 8.0f, 200.0f, 200.0f};
    renderer.push_layer(box, 1.0f);
    renderer.push_rect(box, rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
    if (depth > 1) {
        nest(renderer, depth - 1);
    }
    if (renderer.begin_layer_mask()) {
        renderer.push_rect(box, rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
        renderer.end_layer_mask();
    }
    renderer.pop_layer();
}

bool nested_layers_and_exhaustion() {
    Scene fits;
    fits.frame([&](Batched2DRenderer& renderer) { nest(renderer, 3); });
    bool ok = expect(fits.renderer->offscreen_stats().peak_live == 4,
                     "three nested masked layers hold depth + 1 = 4 targets") &&
              expect(fits.renderer->layer_masks_applied() == 3 &&
                         fits.renderer->offscreen_stats().exhausted == 0,
                     "and fit the default cap of four") &&
              clean(fits, "nested: recycling hazards");

    // Four deep needs five: the innermost mask finds none, is counted, and the layer paints
    // unmasked. Everything else is unaffected.
    Scene over;
    over.frame([&](Batched2DRenderer& renderer) { nest(renderer, 4); });
    const Batched2DRenderer& renderer = *over.renderer;
    ok = expect(renderer.layers_isolated() == 4 && renderer.layers_flat() == 0,
                "past the cap the layers still isolate") &&
         expect(renderer.layer_masks_applied() == 3 && renderer.layer_masks_unsupported() == 1,
                "only the innermost mask falls back") &&
         expect(renderer.offscreen_stats().exhausted == 1 &&
                    renderer.offscreen_stats().exhausted_total == 1,
                "the refused request is counted, per frame and in total") &&
         expect(renderer.offscreen_stats().peak_live == 4, "the pool never exceeded its cap") &&
         clean(over, "over the cap: recycling hazards") && ok;

    // The per-frame counter resets; the total keeps the history.
    over.frame([&](Batched2DRenderer& r) { nest(r, 2); });
    return expect(renderer.offscreen_stats().exhausted == 0 &&
                      renderer.offscreen_stats().exhausted_total == 1,
                  "a clean frame zeroes the per-frame count and keeps the total") &&
           ok;
}

bool small_cap_is_counted() {
    Batched2DConfig config;
    config.max_offscreen_targets = 2;
    Scene scene(config);
    scene.frame([&](Batched2DRenderer& renderer) {
        nest(renderer, 3); // layers 1 and 2 take the pool; the third is flat
    });
    const auto stats = scene.renderer->offscreen_stats();
    return expect(scene.renderer->layers_isolated() == 2 && scene.renderer->layers_flat() == 1,
                  "the layer past the cap paints flat") &&
           expect(stats.exhausted >= 1 && stats.exhausted_total == stats.exhausted,
                  "and the refusal is counted") &&
           expect(stats.targets == 2 && stats.peak_live == 2, "the cap held");
}

bool byte_cap() {
    Batched2DConfig config;
    config.max_offscreen_bytes = kTargetBytes + kTargetBytes / 2; // one target and a half
    Scene scene(config);
    scene.frame([&](Batched2DRenderer& renderer) { masked_card(renderer, 0); });
    const auto stats = scene.renderer->offscreen_stats();
    return expect(scene.renderer->layers_isolated() == 1, "the layer took the one target") &&
           expect(scene.renderer->layer_masks_applied() == 0 &&
                      scene.renderer->layer_masks_unsupported() == 1,
                  "the mask found the byte cap") &&
           expect(stats.exhausted == 1 && stats.bytes == kTargetBytes &&
                      stats.max_bytes == config.max_offscreen_bytes,
                  "refused and counted, and the pool stayed under its bytes");
}

// Steady state over many frames and ring depths: the same two targets serve every frame. The
// window's renderer rings 8 frames (2 per window, 4 windows), and a target is not re-created for
// each of them: before release() four targets lasted four frames and every later frame painted its
// layers flat until the ring wrapped.
bool steady_state() {
    for (const std::uint32_t ring : {1u, 2u, 8u}) {
        Batched2DConfig config;
        config.frames_in_flight = ring;
        Scene scene(config);
        for (int frame = 0; frame < 300; ++frame) {
            scene.frame([&](Batched2DRenderer& renderer) {
                for (int index = 0; index < 12; ++index) {
                    masked_card(renderer, index);
                }
            });
            if (scene.renderer->layer_masks_applied() != 12) {
                return expect(false, "steady: every frame masks all twelve cards");
            }
        }
        const auto stats = scene.renderer->offscreen_stats();
        if (!(expect(stats.targets == 2 && stats.peak_bytes == 2 * kTargetBytes,
                     "300 frames still hold two targets, at every ring depth") &&
              expect(stats.exhausted_total == 0 && stats.peak_live_max == 2, "and never refuse") &&
              clean(scene, "steady: recycling hazards"))) {
            return false;
        }
    }
    return true;
}

bool idle_trim() {
    Batched2DConfig config;
    config.offscreen_idle_trim_frames = 3;
    Scene scene(config);
    scene.frame([&](Batched2DRenderer& renderer) { masked_card(renderer, 0); });
    bool ok = expect(scene.renderer->offscreen_stats().targets == 2, "a masked frame holds two");
    scene.frame([](Batched2DRenderer&) {});
    scene.frame([](Batched2DRenderer&) {});
    ok = expect(scene.renderer->offscreen_stats().targets == 2, "idle for two frames: kept") && ok;
    scene.frame([](Batched2DRenderer&) {});
    ok = expect(scene.renderer->offscreen_stats().targets == 0 &&
                    scene.renderer->offscreen_stats().bytes == 0 &&
                    scene.renderer->offscreen_stats().trimmed_total == 2,
                "idle for three frames: both destroyed, no bytes held") &&
         ok;

    // A static UI draws no more frames, so the driver trims explicitly once its GPU work drained.
    scene.frame([&](Batched2DRenderer& renderer) { masked_card(renderer, 0); });
    scene.renderer->trim_offscreen_targets(false);
    ok = expect(scene.renderer->offscreen_stats().targets == 2,
                "a not-yet-complete frame's targets are kept") &&
         ok;
    scene.renderer->trim_offscreen_targets(true);
    return expect(scene.renderer->offscreen_stats().targets == 0 &&
                      scene.renderer->offscreen_stats().bytes == 0,
                  "a drained driver frees every target") &&
           ok;
}

} // namespace

int main() {
    try {
        const bool ok = sibling_masked_layers(1) && sibling_masked_layers(2) &&
                        sibling_masked_layers(6) && sibling_masked_layers(12) &&
                        hazard_device_catches_early_release() && peak_table() && glass_cards() &&
                        nested_layers_and_exhaustion() && small_cap_is_counted() && byte_cap() &&
                        steady_state() && idle_trim();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_pool_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_pool_null: ok\n";
    return 0;
}
