// SPDX-License-Identifier: MIT
//
// The glass composite (issue #244, docs/specs/glass-v1.md sections 3-5) on the null backend, driven
// through the REAL paint path: paint_scene emits the backdrop draw at the node's paint position,
// the Batched2DSink forwards it, and Batched2DRenderer::draw_backdrop records the whole sequence on
// the frame's command list.
//
// What this pins, deterministically and without a GPU:
//  - one glass node records break -> copy -> horizontal pass -> resume -> composite, in that order
//    (NullCommandStats::command_order), with exactly two pooled targets and two sampled binds;
//  - the bounded-cost fallbacks are real fallbacks: a renderer without glass pipelines, a pool that
//    cannot hand out both targets, and `blur(0)` all paint the frame without the effect and stay
//    attributable (GlassPaintStats), never throwing and never blocking;
//  - a tree without a glass node records exactly today's single-pass stream: no break, no copy, no
//    sampled bind, the same fills.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassLimits;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::null::NullCommandKind;
using tessera::rendering::null::NullCommandStats;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kSize = 200;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_paint_null: " << message << "\n";
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// root(1) background, under(2) background, glass(3) with a child(4): the same scene the paint-pass
// unit test uses, so the emission order is the one that test already pins.
struct Scene {
    Node root{1};
    StubStyles styles;

    explicit Scene(const char* backdrop_filter) {
        root.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
        auto under = std::make_unique<Node>(2);
        under->set_bounds({10.0f, 10.0f, 180.0f, 20.0f});
        root.add_child(std::move(under));
        auto glass = std::make_unique<Node>(3);
        glass->set_bounds({40.0f, 60.0f, 100.0f, 60.0f});
        auto child = std::make_unique<Node>(4);
        child->set_bounds({10.0f, 10.0f, 20.0f, 20.0f});
        glass->add_child(std::move(child));
        root.add_child(std::move(glass));

        styles.set(1, styled({{"background-color", "#202020"}}));
        styles.set(2, styled({{"background-color", "#ff0000"}}));
        styles.set(3, styled({{"background-color", "rgba(255, 255, 255, 0.2)"},
                              {"backdrop-filter", backdrop_filter}}));
        styles.set(4, styled({{"background-color", "#0000ff"}}));
        clear_tree(root);
    }
};

struct Fixture {
    std::unique_ptr<rhi::Device> device = tessera::rendering::null::create_null_device({});
    std::unique_ptr<rhi::RenderTarget> target =
        device->create_render_target(rhi::RenderTargetDesc{{kSize, kSize}});
    std::unique_ptr<rhi::Texture> white = device->create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> blur = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> composite = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::CommandList> commands = device->create_command_list();
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

const NullCommandStats& stats_of(const std::unique_ptr<rhi::CommandList>& commands) {
    return static_cast<const tessera::rendering::null::NullCommandList*>(commands.get())->stats();
}

std::size_t index_of(const NullCommandStats& stats, NullCommandKind kind) {
    for (std::size_t index = 0; index < stats.command_order.size(); ++index) {
        if (stats.command_order[index] == kind) {
            return index;
        }
    }
    return stats.command_order.size();
}

std::size_t last_index_of(const NullCommandStats& stats, NullCommandKind kind) {
    for (std::size_t index = stats.command_order.size(); index > 0; --index) {
        if (stats.command_order[index - 1] == kind) {
            return index - 1;
        }
    }
    return stats.command_order.size();
}

// One frame of `scene` through the real paint path. `config` and `pipelines` control what the
// renderer can do, so the same helper covers the success path and every documented fallback.
struct Painted {
    PaintStats paint;
    std::uint32_t glass_draws = 0;
    std::uint32_t glass_unsupported = 0;
};

Painted paint_frame(Fixture& fixture, Scene& scene, rhi::CommandList& commands, GlassLimits limits,
                    const Batched2DConfig& config = {}, bool with_glass = true,
                    const GlassPipelines& pipelines = {}) {
    GlassPipelines effective = pipelines;
    if (with_glass && effective.blur == nullptr && effective.composite == nullptr) {
        effective = GlassPipelines{fixture.blur.get(), fixture.composite.get()};
    }
    Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, config,
                               effective);
    Batched2DSink sink(renderer);
    PaintContext context;
    context.glass = limits;

    renderer.begin(commands, *fixture.target, target_desc());
    Painted painted;
    painted.paint = paint_scene(scene.root, scene.styles, sink, context);
    renderer.end();
    commands.submit();
    painted.glass_draws = static_cast<std::uint32_t>(sink.glass_stats().draws);
    painted.glass_unsupported = static_cast<std::uint32_t>(sink.glass_stats().unsupported);
    return painted;
}

// One glass node: break -> copy -> horizontal pass -> resume -> composite, then the node's own
// background and child paint over it.
bool composite_order() {
    Fixture fixture;
    Scene scene("blur(4px)");
    GlassLimits limits;
    const Painted painted = paint_frame(fixture, scene, *fixture.commands, limits);
    const NullCommandStats& stats = stats_of(fixture.commands);

    bool ok =
        expect(painted.paint.backdrops == 1, "the paint pass emitted one backdrop") &&
        expect(painted.glass_draws == 1, "the sink executed the composite") &&
        expect(painted.glass_unsupported == 0, "nothing was reported unsupported") &&
        expect(stats.sampled_target_binds == 2, "the horizontal and vertical passes sample") &&
        expect(stats.region_copies == 1, "one dependency-region copy") &&
        expect(stats.begin_render_passes == 3 && stats.end_render_passes == 3,
               "frame, blur target and the resumed frame pass") &&
        // Two fills batch into one draw before the node, the two glass draws, then the two
        // fills after it batch into one: 4 draws, 30 vertices (four quads + two fullscreen).
        expect(stats.draws == 4 && stats.vertices == 30,
               "four fills plus the two glass draws, batched");
    if (!ok) {
        std::cerr << "glass_paint_null: draws=" << stats.draws
                  << " begins=" << stats.begin_render_passes << " copies=" << stats.region_copies
                  << " samples=" << stats.sampled_target_binds << " vertices=" << stats.vertices
                  << " backgrounds=" << painted.paint.backgrounds
                  << " backdrops=" << painted.paint.backdrops << "\n";
        return false;
    }
    const std::size_t copy = index_of(stats, NullCommandKind::CopyTargetRegion);
    const std::size_t first_sample = index_of(stats, NullCommandKind::BindSampledTarget);
    const std::size_t second_sample = last_index_of(stats, NullCommandKind::BindSampledTarget);
    std::size_t draws_after_second_sample = 0;
    for (std::size_t index = second_sample + 1; index < stats.command_order.size(); ++index) {
        draws_after_second_sample += stats.command_order[index] == NullCommandKind::Draw ? 1U : 0U;
    }
    // Everything painted before the glass node is recorded before the break, so the copy reads a
    // frame that already holds it; the composite and the node's own box visuals follow the bind.
    const std::size_t first_draw = index_of(stats, NullCommandKind::Draw);
    return expect(copy < first_sample, "the copy precedes the first sampled bind") &&
           expect(first_sample < second_sample, "the two passes sample in order") &&
           expect(first_draw < copy, "the pre-node geometry is recorded before the break") &&
           expect(draws_after_second_sample == 2,
                  "the composite and the node's own visuals follow the blur");
}

// The whole stream of a frame without a glass node is today's: one pass, no copy, no sampled bind.
bool no_glass_frame_is_unchanged() {
    Fixture fixture;
    Scene scene("none");
    GlassLimits limits;
    const Painted painted = paint_frame(fixture, scene, *fixture.commands, limits);
    const NullCommandStats& stats = stats_of(fixture.commands);
    return expect(painted.paint.backdrops == 0, "no backdrop is emitted") &&
           expect(painted.glass_draws == 0 && painted.glass_unsupported == 0,
                  "the sink sees no glass draw at all") &&
           expect(stats.region_copies == 0, "no copy is recorded") &&
           expect(stats.sampled_target_binds == 0, "no sampled bind is recorded") &&
           expect(stats.begin_render_passes == 1 && stats.end_render_passes == 1,
                  "one pass, as before the feature") &&
           expect(stats.draws == 1 && stats.vertices == 24,
                  "the four fills batch into one draw, exactly as before");
}

// A renderer without glass pipelines paints the node without the effect and counts it.
bool missing_pipelines_is_a_counted_fallback() {
    Fixture fixture;
    Scene scene("blur(4px)");
    GlassLimits limits;
    const Painted painted = paint_frame(fixture, scene, *fixture.commands, limits, {}, false);
    const NullCommandStats& stats = stats_of(fixture.commands);
    return expect(painted.paint.backdrops == 1, "the paint pass still emits the draw") &&
           expect(painted.glass_draws == 0 && painted.glass_unsupported == 1,
                  "the sink reports the draw unsupported") &&
           expect(stats.region_copies == 0 && stats.sampled_target_binds == 0,
                  "nothing extra is recorded") &&
           expect(stats.begin_render_passes == 1, "the frame keeps its single pass") &&
           expect(stats.draws == 1 && stats.vertices == 24,
                  "the node paints its own box visuals, unbundled");
}

// The bounded pool (glassMaxBlurTargets) can only hand out one target, so the composite cannot run;
// the frame still records one pass, draws the node's own visuals and counts the fallback.
bool pool_exhaustion_is_a_counted_fallback() {
    Fixture fixture;
    Scene scene("blur(4px)");
    GlassLimits limits;
    Batched2DConfig config;
    config.max_offscreen_targets = 1;
    const Painted painted = paint_frame(fixture, scene, *fixture.commands, limits, config);
    const NullCommandStats& stats = stats_of(fixture.commands);
    return expect(painted.glass_draws == 0 && painted.glass_unsupported == 1,
                  "the sink reports the pool-bounded fallback") &&
           expect(stats.region_copies == 0, "the copy is not recorded without both targets") &&
           expect(stats.sampled_target_binds == 0, "nothing is sampled") &&
           expect(stats.begin_render_passes == 2 && stats.end_render_passes == 2,
                  "the broken pass is resumed, so the frame stays balanced") &&
           expect(stats.draws == 2 && stats.vertices == 24,
                  "the frame still paints its geometry around the break");
}

// `blur(0)` is a pass-through: the paint pass emits no backdrop at all (glass-v1 section 2).
bool zero_sigma_is_a_pass_through() {
    Fixture fixture;
    Scene scene("blur(0)");
    GlassLimits limits;
    const Painted painted = paint_frame(fixture, scene, *fixture.commands, limits);
    const NullCommandStats& stats = stats_of(fixture.commands);
    return expect(painted.paint.backdrops == 0, "no backdrop for blur(0)") &&
           expect(stats.begin_render_passes == 1 && stats.region_copies == 0,
                  "a pass-through frame is untouched");
}

// The renderer advertises the capability the sink exposes, and the sink's supports_backdrop()
// reports it: a sink without pipelines must not claim it.
bool capability_is_reported() {
    Fixture fixture;
    Batched2DConfig config;
    Batched2DRenderer with_glass(*fixture.device, *fixture.pipeline, *fixture.white, config,
                                 GlassPipelines{fixture.blur.get(), fixture.composite.get()});
    Batched2DSink with_sink(with_glass);
    Batched2DRenderer without_glass(*fixture.device, *fixture.pipeline, *fixture.white, config);
    Batched2DSink without_sink(without_glass);
    Batched2DConfig no_pool = config;
    no_pool.max_offscreen_targets = 0;
    Batched2DRenderer disabled(*fixture.device, *fixture.pipeline, *fixture.white, no_pool,
                               GlassPipelines{fixture.blur.get(), fixture.composite.get()});
    Batched2DSink disabled_sink(disabled);
    return expect(with_sink.supports_backdrop(), "a wired sink supports the backdrop") &&
           expect(!without_sink.supports_backdrop(), "a sink without pipelines does not") &&
           expect(!disabled_sink.supports_backdrop(),
                  "a disabled blur-target pool does not claim the capability");
}

} // namespace

int main() {
    if (!composite_order()) {
        return 1;
    }
    if (!no_glass_frame_is_unchanged()) {
        return 2;
    }
    if (!missing_pipelines_is_a_counted_fallback()) {
        return 3;
    }
    if (!pool_exhaustion_is_a_counted_fallback()) {
        return 4;
    }
    if (!zero_sigma_is_a_pass_through()) {
        return 5;
    }
    if (!capability_is_reported()) {
        return 6;
    }
    std::cout << "glass_paint_null: ok\n";
    return 0;
}
