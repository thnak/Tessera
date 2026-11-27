// SPDX-License-Identifier: MIT
//
// Layer filters (issue #617, docs/design/gpu/offscreen-layers.md section 5.2) on the null backend,
// driven straight through Batched2DRenderer so the recorded command stream is what is asserted:
//  - a blurred layer records content -> horizontal pass (sampled layer, scratch target) -> vertical
//    pass back into the layer -> the ordinary layer composite, with no empty reopened pass;
//  - a drop-shadow adds a third target, composites the layer over it and continues with it;
//  - the filter runs before a mask (filter, then mask, then composite);
//  - every fallback records the group without the effect and counts it: no blur pipeline, no mask
//    pipeline for a shadow, a flat layer, a pool that is out of targets.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
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
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;
using tessera::rendering::null::NullCommandKind;
using tessera::rendering::null::NullCommandStats;
using Kind = NullCommandKind;

constexpr std::uint32_t kSize = 100;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_filter_null: " << message << "\n";
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
    GlassPipelines pipelines(bool with_blur = true, bool with_mask = true) const {
        return GlassPipelines{with_blur ? blur.get() : nullptr, composite.get(),
                              with_mask ? mask.get() : nullptr};
    }
    // The pass/draw/bind skeleton of what was recorded.
    std::vector<Kind> skeleton() const {
        std::vector<Kind> out;
        for (const Kind kind : stats().command_order) {
            if (kind == Kind::BeginRenderPass || kind == Kind::EndRenderPass ||
                kind == Kind::Draw || kind == Kind::BindSampledTarget) {
                out.push_back(kind);
            }
        }
        return out;
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

LayerFilterChain blur_chain(float sigma) {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.sigma = sigma;
    chain.push(op);
    return chain;
}

LayerFilterChain shadow_chain(float sigma) {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.kind = LayerFilterOp::Kind::DropShadow;
    op.offset_x = 4.0f;
    op.offset_y = 4.0f;
    op.sigma = sigma;
    op.set_tint(rhi::Color{0.0f, 0.0f, 0.0f, 0.5f});
    chain.push(op);
    return chain;
}

struct Frame {
    Fixture fixture;
    Batched2DRenderer renderer;
    explicit Frame(Batched2DConfig config = {}, bool with_blur = true, bool with_mask = true)
        : renderer(*fixture.device, *fixture.pipeline, *fixture.white, config,
                   fixture.pipelines(with_blur, with_mask)) {}
};

// One layer with `filter`, one content draw, optionally masked.
void run(Frame& frame, const LayerFilterChain& filter, bool masked) {
    Batched2DRenderer& renderer = frame.renderer;
    renderer.begin(*frame.fixture.commands, *frame.fixture.target, target_desc());
    renderer.push_layer(kBox, 1.0f, filter);
    renderer.push_rect(kBox, kRed);
    if (masked && renderer.begin_layer_mask()) {
        renderer.push_rect({10.0f, 10.0f, 20.0f, 40.0f}, kRed);
        renderer.end_layer_mask();
    }
    renderer.pop_layer();
    renderer.end();
    frame.fixture.commands->submit();
}

bool blur_order() {
    Frame frame;
    run(frame, blur_chain(3.0f), false);
    const std::vector<Kind> want{Kind::BeginRenderPass,   // frame
                                 Kind::EndRenderPass,     // closes at push_layer
                                 Kind::BeginRenderPass,   // layer
                                 Kind::Draw,              // content
                                 Kind::EndRenderPass,     // layer closes for the filter
                                 Kind::BindSampledTarget, // horizontal: the layer
                                 Kind::BeginRenderPass,   // scratch
                                 Kind::Draw,
                                 Kind::EndRenderPass,
                                 Kind::BindSampledTarget, // vertical: the scratch
                                 Kind::BeginRenderPass,   // back into the layer
                                 Kind::Draw,
                                 Kind::EndRenderPass,
                                 Kind::BindSampledTarget, // the layer composite
                                 Kind::BeginRenderPass,   // frame reopened
                                 Kind::Draw,
                                 Kind::EndRenderPass};
    return expect(frame.fixture.skeleton() == want, "blur: pass and draw skeleton") &&
           expect(frame.renderer.layer_filters_applied() == 1 &&
                      frame.renderer.layer_filters_unsupported() == 0,
                  "blur: counted as applied") &&
           expect(frame.renderer.offscreen_target_count() == 2,
                  "blur: the layer and one scratch target");
}

bool shadow_order() {
    Frame frame;
    run(frame, shadow_chain(2.0f), false);
    const std::vector<Kind> want{
        Kind::BeginRenderPass, // frame
        Kind::EndRenderPass,
        Kind::BeginRenderPass, // layer
        Kind::Draw,            // content
        Kind::EndRenderPass,
        Kind::BindSampledTarget, // horizontal: the layer
        Kind::BeginRenderPass,   // scratch
        Kind::Draw,
        Kind::EndRenderPass,
        Kind::BindSampledTarget, // vertical (DestinationIn): the scratch
        Kind::BeginRenderPass,   // shadow target, cleared to the tint
        Kind::Draw,
        Kind::EndRenderPass,
        Kind::BindSampledTarget, // the layer over its shadow
        Kind::BeginRenderPass,   // shadow target reopened (Load)
        Kind::Draw,
        Kind::EndRenderPass,
        Kind::BindSampledTarget, // the shadowed layer composites into the frame
        Kind::BeginRenderPass,
        Kind::Draw,
        Kind::EndRenderPass};
    return expect(frame.fixture.skeleton() == want, "drop-shadow: pass and draw skeleton") &&
           expect(frame.renderer.layer_filters_applied() == 1, "drop-shadow: counted") &&
           expect(frame.renderer.offscreen_target_count() == 3,
                  "drop-shadow: layer, scratch and shadow target");
}

bool filter_precedes_mask() {
    Frame frame;
    run(frame, blur_chain(2.0f), true);
    // Two filter passes (horizontal into the scratch, vertical back into the layer), the mask
    // pass and its composite into the layer, then the layer composite: four sampled binds.
    std::size_t binds = 0;
    for (const Kind kind : frame.fixture.skeleton()) {
        binds += kind == Kind::BindSampledTarget ? 1 : 0;
    }
    return expect(frame.renderer.layer_filters_applied() == 1 &&
                      frame.renderer.layer_masks_applied() == 1,
                  "both the filter and the mask ran") &&
           expect(binds == 4,
                  "horizontal, vertical, mask and layer composite are each one sampled bind") &&
           expect(frame.renderer.offscreen_target_count() == 2 &&
                      frame.renderer.offscreen_stats().peak_live == 2,
                  "layer and scratch; the mask reuses the finished scratch target") &&
           expect(frame.fixture.stats().begin_render_passes ==
                      frame.fixture.stats().end_render_passes,
                  "passes stay balanced");
}

bool counted_fallback(const char* what, Frame& frame, const LayerFilterChain& filter,
                      std::uint32_t isolated, std::uint32_t passes) {
    run(frame, filter, false);
    return expect(frame.renderer.layer_filters_applied() == 0 &&
                      frame.renderer.layer_filters_unsupported() == filter.count,
                  what) &&
           expect(frame.renderer.layers_isolated() == isolated, what) &&
           expect(frame.fixture.stats().begin_render_passes == passes, what) &&
           expect(frame.fixture.stats().begin_render_passes ==
                      frame.fixture.stats().end_render_passes,
                  what);
}

bool fallbacks() {
    Frame no_blur({}, false, true);
    // Isolated and composited as an ordinary layer: frame, layer and the frame reopened.
    bool ok =
        counted_fallback("no blur pipeline skips the filter", no_blur, blur_chain(2.0f), 1, 3);
    Frame no_mask({}, true, false);
    ok = counted_fallback("no mask pipeline skips a drop-shadow", no_mask, shadow_chain(2.0f), 1,
                          3) &&
         ok;
    Fixture fixture;
    Batched2DRenderer flat(*fixture.device, *fixture.pipeline, *fixture.white, {}, {});
    flat.begin(*fixture.commands, *fixture.target, target_desc());
    flat.push_layer(kBox, 0.5f, blur_chain(2.0f));
    flat.pop_layer();
    flat.end();
    fixture.commands->submit();
    ok = expect(flat.layers_flat() == 1 && flat.layer_filters_unsupported() == 1 &&
                    fixture.stats().begin_render_passes == 1,
                "a flat layer drops its filter and records the single pass") &&
         ok;
    Batched2DConfig one;
    one.max_offscreen_targets = 1; // the layer takes it; the scratch finds none
    Frame starved(one);
    ok = counted_fallback("a pool with no scratch target skips the filter", starved,
                          blur_chain(2.0f), 1, 3) &&
         ok;
    return ok;
}

LayerFilterChain color_chain(float amount) {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.set_color_function(0, tessera::rendering::gpu::ColorFunction::Brightness, amount);
    op.set_color_function(1, tessera::rendering::gpu::ColorFunction::Invert, 1.0f);
    chain.push(op);
    return chain;
}

// A run of colour functions is one 1:1 pass: the layer is sampled into the scratch target, which
// becomes the layer, so the chain costs one pass and no blur.
bool color_functions_are_one_pass() {
    Frame frame;
    run(frame, color_chain(1.5f), false);
    const std::vector<Kind> want{
        Kind::BeginRenderPass,   // frame
        Kind::EndRenderPass,     // closes at push_layer
        Kind::BeginRenderPass,   // layer
        Kind::Draw,              // content
        Kind::EndRenderPass,     // layer closes for the filter
        Kind::BindSampledTarget, // the layer
        Kind::BeginRenderPass,   // the scratch target, which becomes the layer
        Kind::Draw,
        Kind::EndRenderPass,
        Kind::BindSampledTarget, // the layer composite
        Kind::BeginRenderPass,   // frame reopened
        Kind::Draw,
        Kind::EndRenderPass};
    bool ok = expect(frame.fixture.skeleton() == want, "colour functions: pass and draw skeleton");
    ok = expect(frame.renderer.layer_filters_applied() == 1 &&
                    frame.renderer.layer_filters_unsupported() == 0,
                "colour functions: counted as applied") &&
         ok;
    ok = expect(frame.renderer.offscreen_target_count() == 2,
                "colour functions: the layer and one destination target") &&
         ok;
    // A blur after the colour pass still works on the swapped target and adds its two passes.
    LayerFilterChain both = color_chain(1.5f);
    both.push(blur_chain(2.0f).ops[0]);
    Frame mixed;
    run(mixed, both, false);
    ok = expect(mixed.renderer.layer_filters_applied() == 2 &&
                    mixed.renderer.offscreen_target_count() == 2 &&
                    mixed.fixture.stats().begin_render_passes ==
                        mixed.fixture.stats().end_render_passes,
                "colour then blur: both applied over two pooled targets") &&
         ok;
    // Without the blur program (the colour pass is that program) or a free target, the layer paints
    // unfiltered and the function is counted.
    Frame no_blur({}, false, true);
    ok = counted_fallback("no program skips the colour functions", no_blur, color_chain(1.5f), 1,
                          3) &&
         ok;
    Batched2DConfig one;
    one.max_offscreen_targets = 1;
    Frame starved(one);
    ok = counted_fallback("a pool with no destination skips the colour functions", starved,
                          color_chain(1.5f), 1, 3) &&
         ok;
    return ok;
}

bool plain_layer_unchanged() {
    Frame frame;
    run(frame, LayerFilterChain{}, false);
    return expect(frame.fixture.stats().begin_render_passes == 3 &&
                      frame.renderer.layer_filters_applied() == 0 &&
                      frame.renderer.layer_filters_unsupported() == 0 &&
                      frame.renderer.offscreen_target_count() == 1,
                  "a layer without a filter records exactly the old stream");
}

bool bounded_over_many_frames() {
    Frame frame;
    for (int index = 0; index < 120; ++index) {
        run(frame, shadow_chain(2.0f), true);
    }
    return expect(frame.renderer.offscreen_target_count() <= 4,
                  "the pool never grows past its bound") &&
           expect(frame.renderer.layer_filters_applied() == 1, "every frame applies the filter");
}

} // namespace

int main() {
    try {
        const bool ok = blur_order() && shadow_order() && color_functions_are_one_pass() &&
                        filter_precedes_mask() && fallbacks() && plain_layer_unchanged() &&
                        bounded_over_many_frames();
        if (!ok) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "layer_filter_null: unexpected exception: " << error.what() << "\n";
        return 2;
    }
    std::cout << "layer_filter_null: ok\n";
    return 0;
}
