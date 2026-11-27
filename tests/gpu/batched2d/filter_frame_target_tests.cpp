// SPDX-License-Identifier: MIT
//
// What a layer `filter` needs from the window's frame target (issue #617,
// docs/design/gpu/offscreen-layers.md section 5d), on the null backend and through the real paint
// path. A glass frame reads the target it paints into (the backdrop is a region copy of it), and a
// swapchain image is neither a copy source nor samplable, so the window renders a glass scene into
// an offscreen frame target and resamples it into the swapchain image. A filter runs entirely on
// pooled layer targets and only composites into the frame, so it must not pay for that detour. The
// window's only question about the tree is `root.glass_subtree()`; this test pins that
//  - the style sync answers it false for a filter-only scene and true for glass,
//  - a filter frame never copies or samples the frame target (no region copy, no blit), while a
//    glass frame copies a region of it, and
//  - the extra frame-sized target and resample a filter would have paid when it was routed like
//    glass are measurable: the recording is the same plus one full-frame blit and one more target.
// It prints the counters so the saving is on record (--verbose-free: always printed, one line each).

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::null::NullCommandStats;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::sync_scene_style;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "filter_frame_target: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

// root(1) > background strip(2) and a card(3) with a child(4); `card_style` carries the effect.
struct Scene {
    Node root{1};
    StubStyles styles;

    Scene(const char* filter, const char* backdrop_filter) {
        root.set_bounds({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)});
        auto strip = std::make_unique<Node>(2);
        strip->set_bounds({100.0f, 100.0f, 1600.0f, 200.0f});
        root.add_child(std::move(strip));
        auto card = std::make_unique<Node>(3);
        card->set_bounds({400.0f, 400.0f, 600.0f, 300.0f});
        auto child = std::make_unique<Node>(4);
        child->set_bounds({40.0f, 40.0f, 200.0f, 100.0f});
        card->add_child(std::move(child));
        root.add_child(std::move(card));
        styles.set(1, styled({{"background-color", "#202020"}}));
        styles.set(2, styled({{"background-color", "#ff0000"}}));
        styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                              {"filter", filter},
                              {"backdrop-filter", backdrop_filter}}));
        styles.set(4, styled({{"background-color", "#0000ff"}}));
        sync_scene_style(root, styles, {});
    }
};

struct Counters {
    NullCommandStats stats;
    std::uint32_t frame_targets = 1; // the frame target(s) the window allocates per slot
    std::uint64_t blit_bytes = 0;    // read + write of every full-frame resample
};

constexpr std::uint64_t kFrameBytes = static_cast<std::uint64_t>(kWidth) * kHeight * 4;

// Records one frame of `scene`. `offscreen` reproduces the window's glass route: the scene paints
// into an offscreen frame target and the same list resamples it into the presented image.
Counters record(Scene& scene, bool offscreen) {
    auto device = tessera::rendering::null::create_null_device({});
    rhi::RenderTargetDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.clear_color = rhi::rgba8(0, 0, 0);
    auto presented = device->create_render_target(desc);
    auto frame = offscreen ? device->create_render_target(desc) : nullptr;
    auto white = device->create_texture(rhi::TextureDesc{{1, 1}});
    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto blur = device->create_pipeline(rhi::PipelineDesc{});
    auto composite = device->create_pipeline(rhi::PipelineDesc{});
    rhi::PipelineDesc mask_desc;
    mask_desc.blend = rhi::BlendMode::DestinationIn;
    auto mask = device->create_pipeline(mask_desc);
    auto commands = device->create_command_list();

    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{},
                               GlassPipelines{blur.get(), composite.get(), mask.get()});
    Batched2DSink sink(renderer);
    rhi::RenderTarget& target = offscreen ? *frame : *presented;
    renderer.begin(*commands, target, desc);
    paint_scene(scene.root, scene.styles, sink, PaintContext{});
    renderer.end();
    Counters counters;
    if (offscreen) {
        commands->blit_target(*frame, *presented, rhi::Filter::Nearest);
        counters.frame_targets = 2;
        counters.blit_bytes = 2 * kFrameBytes;
    }
    commands->submit();
    counters.stats = static_cast<const NullCommandList*>(commands.get())->stats();
    return counters;
}

void print(const char* label, const Counters& c) {
    std::cout << "filter_frame_target: " << label << ": passes=" << c.stats.begin_render_passes
              << " draws=" << c.stats.draws << " sampled_binds=" << c.stats.sampled_target_binds
              << " region_copies=" << c.stats.region_copies << " blits=" << c.stats.blits
              << " frame_targets=" << c.frame_targets
              << " frame_target_MB=" << (c.frame_targets * kFrameBytes) / (1024.0 * 1024.0)
              << " resample_MB=" << c.blit_bytes / (1024.0 * 1024.0) << "\n";
}

bool the_window_sees_glass_only() {
    const Scene plain("none", "none");
    const Scene filter("blur(8px)", "none");
    const Scene shadow("drop-shadow(6px 6px 4px black)", "none");
    const Scene glass("none", "blur(8px)");
    const Scene both("blur(8px)", "blur(8px)");
    return expect(!plain.root.glass_subtree(), "no effect: no offscreen frame target") &&
           expect(!filter.root.glass_subtree() && filter.root.filter_subtree(),
                  "a blur filter does not ask for the offscreen frame target") &&
           expect(!shadow.root.glass_subtree() && shadow.root.filter_subtree(),
                  "neither does a drop-shadow") &&
           expect(glass.root.glass_subtree() && !glass.root.filter_subtree(),
                  "a backdrop blur still does") &&
           expect(both.root.glass_subtree() && both.root.filter_subtree(),
                  "a node with both does, through the glass flag alone");
}

bool a_filter_frame_never_touches_the_frame_target() {
    Scene filter("blur(8px)", "none");
    Scene glass("none", "blur(8px)");
    const Counters direct = record(filter, false);
    const Counters backdrop = record(glass, false);
    bool ok = expect(direct.stats.region_copies == 0 && direct.stats.blits == 0,
                     "a filter frame copies and resamples nothing of the frame target") &&
              expect(direct.stats.sampled_target_binds >= 2,
                     "it samples only its pooled layer targets (blur passes)") &&
              expect(direct.stats.begin_render_passes >= 4,
                     "and still runs its own layer, blur and composite passes") &&
              expect(backdrop.stats.region_copies == 1,
                     "a glass frame copies a region of the frame target");
    return ok;
}

bool routing_a_filter_like_glass_cost_a_target_and_a_resample() {
    Scene scene("blur(8px)", "none");
    const Counters direct = record(scene, false);
    const Counters detour = record(scene, true);
    print("filter, direct frame target (after) ", direct);
    print("filter, offscreen + resample (before)", detour);
    return expect(detour.stats.begin_render_passes == direct.stats.begin_render_passes &&
                      detour.stats.draws == direct.stats.draws &&
                      detour.stats.sampled_target_binds == direct.stats.sampled_target_binds,
                  "the render work is identical either way") &&
           expect(detour.stats.blits == direct.stats.blits + 1 &&
                      detour.frame_targets == direct.frame_targets + 1,
                  "the detour is exactly one more frame target and one full-frame resample") &&
           expect(detour.blit_bytes == 2 * kFrameBytes && direct.blit_bytes == 0,
                  "which is a frame of reads and a frame of writes every presented frame");
}

} // namespace

int main() {
    const bool ok = the_window_sees_glass_only() && a_filter_frame_never_touches_the_frame_target() &&
                    routing_a_filter_like_glass_cost_a_target_and_a_resample();
    return ok ? 0 : 1;
}
