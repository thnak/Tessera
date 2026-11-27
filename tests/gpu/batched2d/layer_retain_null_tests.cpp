// SPDX-License-Identifier: MIT
//
// Retained layers (issue #617, docs/design/gpu/offscreen-layers.md section 5.6) on the null
// backend, driven through Batched2DRenderer::retain_layer():
//  - the first clean frame renders each layer and copies its finished pixels into a retained
//  target;
//    the next frames composite that target and draw nothing for the subtree, the mask or the
//    filter (counted in passes, draws, sampled binds and region copies, against an uncached
//    control that renders every layer every frame);
//  - a node reported dirty drops its entry and renders again, a changed key misses, and only the
//    changed node does either - the others keep hitting;
//  - the recorded region is the layer's whole box even when the frame is damage-clipped, and a
//    later damage-clipped hit composites only the damage;
//  - the byte and entry caps refuse or evict deterministically, entries used this frame are never
//    evicted for a newcomer, and the bytes are charged to the pool's own byte cap;
//  - a layer whose content degraded is not kept, a resize drops every entry, an explicit trim and
//    the idle window give the memory back at every ring depth, and the stream stays free of
//    recycling hazards.

#include "layer_hazard_device.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
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
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;
constexpr std::uint64_t kKey = 0xabcdef12u;
constexpr std::uint64_t kCardBytes = 36ull * 36 * 4;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_retain_null: " << message << "\n";
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
    std::unique_ptr<Batched2DRenderer> renderer;
    tessera::rendering::null::NullCommandStats last;
    std::uint32_t width = kWidth;
    std::uint32_t height = kHeight;

    explicit Scene(Batched2DConfig config = {}) {
        target = device.create_render_target(rhi::RenderTargetDesc{{kWidth, kHeight}});
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

// One masked card. `retain` < 0 is the uncached control: retain_layer() is never asked.
// `clean` is what the paint pass would say about the node; `key` what it folded.
bool masked_card(Batched2DRenderer& renderer, int index, bool retain = true, bool clean = true,
                 std::uint64_t key = kKey, const LayerFilterChain& filter = {}) {
    const rhi::Rect box = card(index);
    bool hit = false;
    if (retain) {
        hit = renderer.retain_layer(static_cast<std::uint64_t>(index + 1), key, clean);
    }
    renderer.push_layer(box, 1.0f, filter);
    if (!hit) {
        renderer.push_rect(box, rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
        if (renderer.begin_layer_mask()) {
            renderer.push_rect({box.x, box.y, box.width * 0.5f, box.height},
                               rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
            renderer.end_layer_mask();
        }
    }
    renderer.pop_layer();
    return hit;
}

LayerFilterChain blur_chain() {
    LayerFilterChain chain;
    chain.count = 1;
    chain.ops[0].kind = LayerFilterOp::Kind::Blur;
    chain.ops[0].sigma = 2.0f;
    return chain;
}

bool clean_stream(const Scene& scene, const char* what) {
    return expect(scene.log.lost_writes == 0, what) &&
           expect(scene.log.sampled_pass_conflicts == 0, what) &&
           expect(scene.log.unsampled_targets() == 0, what) &&
           expect(scene.last.begin_render_passes == scene.last.end_render_passes, what);
}

// Six masked cards: frame 1 renders and records, frames 2 and 3 only composite.
bool record_then_hit() {
    Scene scene;
    const auto cards = [](Batched2DRenderer& renderer) {
        std::uint32_t hits = 0;
        for (int index = 0; index < 6; ++index) {
            hits += masked_card(renderer, index) ? 1u : 0u;
        }
        return hits;
    };
    std::uint32_t hits = 0;
    scene.frame([&](Batched2DRenderer& renderer) { hits = cards(renderer); });
    const auto first = scene.last;
    bool ok =
        expect(hits == 0, "frame 1: nothing retained yet") &&
        expect(scene.renderer->retained_layer_stats().records == 6 &&
                   scene.renderer->retained_layer_stats().entries == 6,
               "frame 1: six layers recorded") &&
        expect(first.region_copies == 6, "frame 1: one region copy per layer") &&
        expect(first.target_region_copies.size() == 6 &&
                   first.target_region_copies[1].source_region.x == 48 &&
                   first.target_region_copies[1].source_region.y == 8 &&
                   first.target_region_copies[1].source_region.width == 36 &&
                   first.target_region_copies[1].source_region.height == 36 &&
                   first.target_region_copies[1].destination_offset.x == 0 &&
                   first.target_region_copies[1].destination_extent.width == 36 &&
                   first.target_region_copies[1].destination_extent.height == 36,
               "the copy is the card's box, into an exact-size target at its origin") &&
        expect(scene.renderer->layers_isolated() == 6, "frame 1: rendered as isolated layers") &&
        clean_stream(scene, "frame 1: recycling hazards");

    // Control: the same frame with retention never asked, to price what a hit saves.
    Scene control;
    control.frame([&](Batched2DRenderer& renderer) {
        for (int index = 0; index < 6; ++index) {
            (void)masked_card(renderer, index, false);
        }
    });

    for (int frame = 2; frame <= 3; ++frame) {
        scene.frame([&](Batched2DRenderer& renderer) { hits = cards(renderer); });
        const auto& now = scene.last;
        ok = expect(hits == 6, "later frames hit every layer") &&
             expect(scene.renderer->layers_isolated() == 0 && scene.renderer->layers_flat() == 0,
                    "a hit opens no layer pass") &&
             expect(now.region_copies == 0, "a hit copies nothing") &&
             expect(now.draws == 6, "one composite draw per layer and no content draw") &&
             expect(now.sampled_target_binds == 6, "each retained target is sampled once") &&
             // the frame pass, reopened after each composite
             expect(now.begin_render_passes == 1 + 6, "no layer or mask pass was recorded") &&
             expect(scene.renderer->layer_masks_applied() == 0,
                    "the mask is not run again either") &&
             clean_stream(scene, "hit frame: recycling hazards") && ok;
    }
    ok = expect(control.last.draws > 3 * scene.last.draws &&
                    control.last.begin_render_passes > 3 * scene.last.begin_render_passes,
                "the uncached control draws and passes far more") &&
         expect(scene.renderer->retained_layer_stats().hits == 12 &&
                    scene.renderer->retained_layer_stats().records == 6,
                "twelve hits over two frames, still six records") &&
         ok;
    const auto pool = scene.renderer->offscreen_stats();
    return expect(pool.bytes == pool.targets * 1920ull * 1080 * 4 + 6 * kCardBytes,
                  "the retained bytes are charged to the pool's byte cap") &&
           ok;
}

// A dirty node drops its entry; a clean frame after it records again; the others never notice.
bool invalidation() {
    Scene scene;
    const auto run = [&](bool dirty_three, std::uint64_t key_four) {
        std::uint32_t hits = 0;
        scene.frame([&](Batched2DRenderer& renderer) {
            for (int index = 0; index < 6; ++index) {
                const bool dirty = index == 2 && dirty_three;
                const std::uint64_t key = index == 3 ? key_four : kKey;
                hits += masked_card(renderer, index, true, !dirty, key) ? 1u : 0u;
            }
        });
        return hits;
    };
    bool ok = expect(run(false, kKey) == 0, "frame 1 records") &&
              expect(run(false, kKey) == 6, "frame 2 hits");
    const auto before = scene.renderer->retained_layer_stats();
    ok = expect(run(true, kKey) == 5, "a dirty node renders again, the other five still hit") &&
         expect(scene.renderer->retained_layer_stats().invalidated == before.invalidated + 1 &&
                    scene.renderer->retained_layer_stats().entries == 5,
                "and its entry is gone") &&
         expect(scene.last.region_copies == 0, "a dirty node is not copied: it is changing") &&
         expect(scene.renderer->layers_isolated() == 1, "only the dirty layer rendered") &&
         expect(run(false, kKey) == 5,
                "the first clean frame after the change misses and records") &&
         expect(scene.last.region_copies == 1, "...by one copy") &&
         expect(run(false, kKey) == 6, "and the next frame hits again") && ok;
    // A key that moved (a different transform, clip or opacity product) is a miss and replaces
    // the entry instead of adding one.
    ok = expect(run(false, kKey + 1) == 5, "a changed key misses") &&
         expect(scene.renderer->retained_layer_stats().entries == 6,
                "and the owner keeps one entry") &&
         expect(run(false, kKey + 1) == 6, "the new key hits next") && ok;
    return clean_stream(scene, "invalidation: recycling hazards") && ok;
}

// The kept pixels are the layer's whole box, whatever the frame's damage.
bool damage_clipped_record_and_hit() {
    Scene scene;
    const std::vector<rhi::Scissor> left{{0, 0, 20, 100}};   // the left part of card 0 only
    const std::vector<rhi::Scissor> right{{30, 0, 40, 100}}; // the right part of card 0
    scene.frame([](Batched2DRenderer& renderer) { (void)masked_card(renderer, 0); }, &left);
    const auto first = scene.last;
    bool ok =
        expect(first.region_copies == 1 && first.target_region_copies[0].source_region.x == 8 &&
                   first.target_region_copies[0].source_region.width == 36,
               "the record under damage covers the whole box") &&
        expect(!first.draw_records.empty(), "the frame drew");
    bool content_full = false;
    for (const auto& record : first.draw_records) {
        content_full = content_full || (record.scissor.x == 8 && record.scissor.width == 36);
    }
    ok = expect(content_full, "the layer's content was not cut to the damage") && ok;

    bool hit = false;
    scene.frame([&](Batched2DRenderer& renderer) { hit = masked_card(renderer, 0); }, &right);
    const auto second = scene.last;
    ok = expect(hit, "a different damage still hits") &&
         expect(second.draws == 1 && second.draw_records[0].scissor.x == 30 &&
                    second.draw_records[0].scissor.width == 14,
                "the composite is cut to the damage: [30, 44)") &&
         clean_stream(scene, "damage: recycling hazards") && ok;
    // Outside the damage entirely: nothing to composite, nothing recorded.
    const std::vector<rhi::Scissor> elsewhere{{1000, 500, 10, 10}};
    scene.frame([&](Batched2DRenderer& renderer) { hit = masked_card(renderer, 0); }, &elsewhere);
    return expect(hit && scene.last.draws == 0 && scene.last.begin_render_passes == 1,
                  "a hit outside the damage records no pass and no draw") &&
           ok;
}

bool budgets() {
    // Entry cap: two of six fit; the rest are refused, never evicting what this frame used.
    Batched2DConfig two;
    two.max_retained_layers = 2;
    Scene capped(two);
    const auto run = [&](Scene& scene) {
        std::uint32_t hits = 0;
        scene.frame([&](Batched2DRenderer& renderer) {
            for (int index = 0; index < 6; ++index) {
                hits += masked_card(renderer, index) ? 1u : 0u;
            }
        });
        return hits;
    };
    bool ok =
        expect(run(capped) == 0 && capped.renderer->retained_layer_stats().entries == 2 &&
                   capped.renderer->retained_layer_stats().refused == 4,
               "entry cap: two kept, four refused") &&
        expect(run(capped) == 2 && capped.renderer->retained_layer_stats().evicted == 0,
               "the working set above the cap does not thrash: the same two keep hitting") &&
        expect(capped.last.region_copies == 0, "refused layers draw normally and copy nothing");

    // Byte cap: room for two cards (a card is 36 x 36 x 4).
    Batched2DConfig bytes;
    bytes.max_retained_bytes = 2 * kCardBytes + 100;
    Scene small(bytes);
    ok = expect(run(small) == 0 && small.renderer->retained_layer_stats().entries == 2 &&
                    small.renderer->retained_layer_stats().bytes == 2 * kCardBytes,
                "byte cap: two cards kept") &&
         ok;

    // LRU: with room for two, card 3 evicts the entry that was not used this frame.
    Scene lru(two);
    lru.frame([](Batched2DRenderer& renderer) {
        (void)masked_card(renderer, 0);
        (void)masked_card(renderer, 1);
    });
    lru.frame([](Batched2DRenderer& renderer) { (void)masked_card(renderer, 0); }); // 0 is newest
    lru.frame([](Batched2DRenderer& renderer) { (void)masked_card(renderer, 2); });
    bool zero = false;
    bool one = true;
    bool two_hit = false;
    lru.frame([&](Batched2DRenderer& renderer) {
        zero = masked_card(renderer, 0);
        two_hit = masked_card(renderer, 2);
    });
    lru.frame([&](Batched2DRenderer& renderer) { one = masked_card(renderer, 1); });
    ok = expect(!one, "the least recently used entry was evicted for the newcomer") &&
         expect(zero && two_hit, "the recently used entries survived") &&
         expect(lru.renderer->retained_layer_stats().evicted >= 1, "and the eviction is counted") &&
         ok;

    // The pool's own byte cap refuses a record before the retained share does.
    Batched2DConfig tight;
    tight.max_offscreen_bytes = 2ull * 1920 * 1080 * 4 + kCardBytes; // two live targets, one card
    Scene pool(tight);
    ok = expect(run(pool) == 0 && pool.renderer->retained_layer_stats().entries == 1 &&
                    pool.renderer->offscreen_stats().exhausted == 0 &&
                    pool.renderer->layer_masks_applied() == 6,
                "the pool cap leaves the live layers whole and keeps only what fits") &&
         ok;
    return clean_stream(pool, "budget: recycling hazards") && ok;
}

// What a sink reports as degraded is never kept.
bool degraded_layer_is_not_kept() {
    Scene scene;
    scene.frame([](Batched2DRenderer& renderer) {
        (void)renderer.retain_layer(1, kKey, true);
        renderer.push_layer(card(0), 1.0f);
        renderer.push_rect(card(0), rhi::Color{1.0f, 0.0f, 0.0f, 1.0f});
        renderer.note_layer_degraded(); // an image that is still loading
        renderer.pop_layer();
    });
    bool hit = true;
    scene.frame([&](Batched2DRenderer& renderer) { hit = masked_card(renderer, 0); });
    return expect(scene.renderer->retained_layer_stats().records == 1 &&
                      scene.renderer->retained_layer_stats().entries == 1 && !hit,
                  "a degraded layer copies nothing and drops its reservation; the next clean one "
                  "records") &&
           clean_stream(scene, "degraded: recycling hazards");
}

bool resize_and_trim() {
    Scene scene;
    const auto run = [&] {
        bool hit = false;
        scene.frame([&](Batched2DRenderer& renderer) { hit = masked_card(renderer, 0); });
        return hit;
    };
    bool ok = expect(!run() && run(), "recorded, then hit");
    // A new frame size changes every pixel region: nothing may be reused.
    scene.width = 1280;
    scene.height = 720;
    scene.target = scene.device.create_render_target(rhi::RenderTargetDesc{{1280, 720}});
    scene.log.ignore(scene.target.get());
    ok = expect(!run(), "a resize drops every entry") &&
         expect(scene.renderer->retained_layer_stats().evicted >= 1, "and counts the eviction") &&
         expect(run(), "the new size records and hits again") && ok;

    scene.renderer->trim_offscreen_targets(false);
    ok = expect(scene.renderer->retained_layer_stats().entries == 0,
                "an explicit trim releases the entries") &&
         ok;
    scene.renderer->trim_offscreen_targets(true);
    ok = expect(scene.renderer->offscreen_stats().bytes == 0,
                "a drained driver holds no layer byte at all, retained ones included") &&
         ok;
    return expect(!run(), "after a trim the layer records again") && ok;
}

bool idle_release_and_rings() {
    for (const std::uint32_t ring : {1u, 2u, 8u}) {
        Batched2DConfig config;
        config.frames_in_flight = ring;
        config.offscreen_idle_trim_frames = 12;
        Scene scene(config);
        for (int frame = 0; frame < 40; ++frame) {
            std::uint32_t hits = 0;
            scene.frame([&](Batched2DRenderer& renderer) {
                for (int index = 0; index < 4; ++index) {
                    hits += masked_card(renderer, index) ? 1u : 0u;
                }
            });
            if (!expect(hits == (frame == 0 ? 0u : 4u), "steady frames hit at every ring depth")) {
                return false;
            }
        }
        const auto steady = scene.renderer->offscreen_stats();
        for (int frame = 0; frame < 40; ++frame) {
            scene.frame([](Batched2DRenderer&) {});
        }
        if (!(expect(scene.renderer->retained_layer_stats().entries == 0 &&
                         scene.renderer->offscreen_stats().bytes == 0,
                     "idle frames release every entry and every byte") &&
              expect(steady.exhausted_total == 0, "and nothing was refused") &&
              clean_stream(scene, "rings: recycling hazards"))) {
            return false;
        }
    }
    return true;
}

// A blurred card grows by 3 sigma: the retained region is the grown box, and a hit needs no filter.
bool filtered_layer() {
    Scene scene;
    scene.frame([](Batched2DRenderer& renderer) {
        (void)masked_card(renderer, 0, true, true, kKey, blur_chain());
    });
    const auto first = scene.last;
    bool ok =
        expect(first.region_copies == 1 && first.target_region_copies[0].source_region.x == 8 - 6 &&
                   first.target_region_copies[0].source_region.width == 36 + 12,
               "the kept region includes the filter's halo (3 sigma = 6 px)") &&
        expect(scene.renderer->layer_filters_applied() == 1, "frame 1 ran the blur");
    bool hit = false;
    scene.frame([&](Batched2DRenderer& renderer) {
        hit = masked_card(renderer, 0, true, true, kKey, blur_chain());
    });
    return expect(hit && scene.last.draws == 1 && scene.renderer->layer_filters_applied() == 0,
                  "a hit runs no filter pass: one composite draw") &&
           clean_stream(scene, "filter: recycling hazards") && ok;
}

} // namespace

int main() {
    try {
        const bool ok = record_then_hit() && invalidation() && damage_clipped_record_and_hit() &&
                        budgets() && degraded_layer_is_not_kept() && resize_and_trim() &&
                        idle_release_and_rings() && filtered_layer();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_retain_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_retain_null: ok\n";
    return 0;
}
