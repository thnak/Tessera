// SPDX-License-Identifier: MIT

#include "benchmark_fixture.hpp"
#include "cpu_benchmark_assets.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace tessera::benchmark {
namespace {

namespace image = tessera::rendering::image;
namespace nullrhi = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kTargetWidth = static_cast<std::uint32_t>(kBenchmarkWidth);
constexpr std::uint32_t kTargetHeight = static_cast<std::uint32_t>(kBenchmarkHeight);

[[nodiscard]] double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

[[nodiscard]] ui::paint::ImagePaintConfig image_config(ui::assets::AssetBundle& bundle,
                                                       image::ImageDecodeCache& decoded,
                                                       image::ImageTextureCache& textures) {
    ui::paint::ImagePaintConfig config;
    config.bundle = &bundle;
    config.decoded = &decoded;
    config.textures = &textures;
    config.device_scale = 1.0f;
    return config;
}

} // namespace

Fixture::Fixture()
    : root_(std::make_unique<ui::Node>(1)),
      device_(nullrhi::create_null_device({})),
      target_(device_->create_render_target({{kTargetWidth, kTargetHeight}})),
      white_(device_->create_texture({{1, 1}})),
      pipeline_(device_->create_pipeline({})),
      glass_blur_pipeline_(device_->create_pipeline({})),
      glass_composite_pipeline_(device_->create_pipeline({})),
      commands_(device_->create_command_list()),
      renderer_(std::make_unique<rendering::gpu::Batched2DRenderer>(
          *device_, *pipeline_, *white_, rendering::gpu::Batched2DConfig{},
          rendering::gpu::GlassPipelines{glass_blur_pipeline_.get(),
                                         glass_composite_pipeline_.get()})),
      bundle_(fixtures::bundle()),
      textures_(*device_),
      batched_(*renderer_, nullptr, nullptr, {}, image_config(bundle_, decoded_, textures_)),
      sink_(batched_) {
    targets_ = builder_.build(*root_);
    const ui::layout::LayoutViewport viewport{kBenchmarkWidth, kBenchmarkHeight, 1.0f};
    driver_ = std::make_unique<BenchmarkDriver>(*root_, compile_benchmark_stylesheet(), viewport);
}

Fixture::~Fixture() = default;

PaintOutcome Fixture::paint(bool force, const rendering::RedrawPlan* plan) {
    sink_.reset();
    const rhi::RenderTargetDesc desc{{kTargetWidth, kTargetHeight}};
    const bool partial = plan != nullptr && !plan->full_redraw;
    const Clock::time_point begin = Clock::now();
    if (partial) {
        renderer_->begin_damaged(*commands_, *target_, desc, *plan);
    } else {
        renderer_->begin(*commands_, *target_, desc);
    }
    const ui::frame::FramePaint painted = driver_->paint(sink_, force);
    renderer_->end();
    PaintOutcome outcome;
    outcome.ms = milliseconds(begin, Clock::now());
    if (!painted.painted) {
        throw std::runtime_error("benchmark: the frame due after begin_frame() was not painted");
    }
    outcome.draw_calls = renderer_->draw_calls();
    outcome.quads = renderer_->vertex_count() / 6u;
    outcome.quads_culled = renderer_->cull_stats().quads_culled;
    const DrawListCounts& counts = sink_.counts();
    outcome.primitives = counts.primitives();
    outcome.text_runs = counts.text_runs;
    outcome.images = counts.images;
    outcome.backdrops = counts.backdrops;
    outcome.paint_nodes = painted.stats.nodes;
    outcome.culled_nodes = painted.stats.culled_nodes;
    outcome.culled_subtrees = painted.stats.culled_subtrees;
    return outcome;
}

// The glass scenario (issue #244, glass-v1 section 9): the fixed top bar gets a `backdrop-filter`
// through an inline style, which is the same path a stylesheet declaration takes into the paint
// pass. No node is added, so the reference tree stays the 12 000-node scene the budget pins.
void Fixture::enable_glass_bar() {
    if (targets_.glass_bar == nullptr) {
        throw std::runtime_error("benchmark: the scene has no glass bar");
    }
    (void)targets_.glass_bar->set_inline_style("backdrop-filter", "blur(16px)");
    targets_.glass_bar->mark_dirty(ui::Dirty::Style | ui::Dirty::Paint);
}

void Fixture::edit_text(const char* text) {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = text != nullptr
                    ? text
                    : "Card 0 summary title, rewritten by the benchmark to force a text remeasure";
    targets_.text_node->set_paint_data(std::move(data));
    // A paint payload never marks layout by itself; the app owns the decision that a text change
    // can move boxes.
    targets_.text_node->mark_dirty(ui::Dirty::Layout);
}

void Fixture::edit_container() {
    targets_.container_node->set_inline_style("width", "260px");
}

Measurement Fixture::measure() {
    Measurement result;
    // The first frame of a fresh scene is the cold style+layout path. Force the redraw plan to a
    // full target so the paint below is a full paint and its counters describe the whole tree.
    driver_->request_full_repaint();
    Clock::time_point begin = Clock::now();
    const FrameWork first = driver_->begin_frame();
    result.first_style_layout_ms = milliseconds(begin, Clock::now());
    result.damage_rects_full = first.render.redraw_plan.scissors.size();
    const PaintOutcome full = paint(false);
    result.full_paint_ms = full.ms;
    result.draw_calls = full.draw_calls;
    result.quads = full.quads;
    result.quads_culled = full.quads_culled;
    result.primitives = full.primitives;
    result.text_runs = full.text_runs;
    result.images = full.images;
    result.paint_nodes = full.paint_nodes;
    driver_->end_frame();

    edit_text();
    begin = Clock::now();
    const FrameWork text_frame = driver_->begin_frame();
    result.text_phases = driver_->phases();
    (void)paint(false);
    result.layout_paint_text_ms = milliseconds(begin, Clock::now());
    result.damage_rects_text = text_frame.render.redraw_plan.scissors.size();
    driver_->end_frame();

    edit_container();
    begin = Clock::now();
    const FrameWork container_frame = driver_->begin_frame();
    (void)paint(false);
    result.layout_paint_container_ms = milliseconds(begin, Clock::now());
    result.damage_rects_container = container_frame.render.redraw_plan.scissors.size();
    driver_->end_frame();

    // Damaged frame (issue #474): a fresh one-node text edit, painted through `begin_damaged` so
    // the CPU culling tests against the plan's damage bounding box instead of the whole target.
    // Only the paint is timed; the counters show what the damaged path drops, which on this scene
    // is nothing beyond the paint pass's own clip. A text that actually differs is required:
    // re-applying the same payload is a true no-op frame (no damage, no paint).
    edit_text("Card 0 summary title, edited again so exactly one node is damaged");
    const FrameWork damaged_frame = driver_->begin_frame();
    const rendering::RedrawPlan damaged_plan = damaged_frame.render.redraw_plan;
    result.damaged_partial = !damaged_plan.full_redraw;
    result.damaged_damage_rects = damaged_plan.scissors.size();
    const PaintOutcome damaged = paint(false, &damaged_plan);
    result.damaged_paint_ms = damaged.ms;
    result.damaged_quads = damaged.quads;
    result.damaged_quads_culled = damaged.quads_culled;
    result.damaged_draw_calls = damaged.draw_calls;
    result.damaged_paint_nodes = damaged.paint_nodes;
    result.damaged_culled_nodes = damaged.culled_nodes;
    result.damaged_culled_subtrees = damaged.culled_subtrees;
    driver_->end_frame();

    // Repeated full repaint of a clean tree (issue #476): the first one records the retained draw
    // lists, the second replays them. This is the frame a resize-free expose, a forced repaint or a
    // swapchain recreate pays when nothing in the scene changed.
    for (int repeat = 0; repeat < 2; ++repeat) {
        driver_->request_full_repaint();
        (void)driver_->begin_frame();
        (void)paint(false);
        driver_->end_frame();
    }
    driver_->request_full_repaint();
    (void)driver_->begin_frame();
    const PaintOutcome repeated = paint(false);
    result.repeat_paint_ms = repeated.ms;
    result.repeat_paint_nodes = repeated.paint_nodes;
    driver_->end_frame();

    // Glass (issue #244): the same full paint with the fixed bar blurred. The first glass frame
    // also creates the two pooled blur targets, which is a frame-boundary cost, not a per-frame
    // one; the measured frame is the steady state a scrolling screen under the bar pays, and it
    // records the copy, both blur passes and the composite through the null RHI so the CPU cost is
    // real.
    enable_glass_bar();
    driver_->request_full_repaint();
    (void)driver_->begin_frame();
    (void)paint(false);
    driver_->end_frame();
    driver_->request_full_repaint();
    begin = Clock::now();
    (void)driver_->begin_frame();
    const PaintOutcome glass = paint(false);
    result.glass_paint_ms = milliseconds(begin, Clock::now());
    result.glass_backdrops = glass.backdrops;
    driver_->end_frame();
    return result;
}

} // namespace tessera::benchmark
