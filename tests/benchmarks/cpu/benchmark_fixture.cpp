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
      commands_(device_->create_command_list()),
      renderer_(std::make_unique<rendering::gpu::Batched2DRenderer>(*device_, *pipeline_, *white_)),
      bundle_(fixtures::bundle()),
      textures_(*device_),
      batched_(*renderer_, nullptr, nullptr, {}, image_config(bundle_, decoded_, textures_)),
      sink_(batched_) {
    targets_ = builder_.build(*root_);
    const ui::layout::LayoutViewport viewport{kBenchmarkWidth, kBenchmarkHeight, 1.0f};
    driver_ = std::make_unique<BenchmarkDriver>(*root_, compile_benchmark_stylesheet(), viewport);
}

Fixture::~Fixture() = default;

PaintOutcome Fixture::paint(bool force) {
    sink_.reset();
    const rhi::RenderTargetDesc desc{{kTargetWidth, kTargetHeight}};
    const Clock::time_point begin = Clock::now();
    renderer_->begin(*commands_, *target_, desc);
    const ui::frame::FramePaint painted = driver_->paint(sink_, force);
    renderer_->end();
    PaintOutcome outcome;
    outcome.ms = milliseconds(begin, Clock::now());
    if (!painted.painted) {
        throw std::runtime_error("benchmark: the frame due after begin_frame() was not painted");
    }
    outcome.draw_calls = renderer_->draw_calls();
    outcome.quads = renderer_->vertex_count() / 6u;
    const DrawListCounts& counts = sink_.counts();
    outcome.primitives = counts.primitives();
    outcome.text_runs = counts.text_runs;
    outcome.images = counts.images;
    outcome.paint_nodes = painted.stats.nodes;
    return outcome;
}

void Fixture::edit_text() {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = "Card 0 summary title, rewritten by the benchmark to force a text remeasure";
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
    result.primitives = full.primitives;
    result.text_runs = full.text_runs;
    result.images = full.images;
    result.paint_nodes = full.paint_nodes;
    driver_->end_frame();

    edit_text();
    begin = Clock::now();
    const FrameWork text_frame = driver_->begin_frame();
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
    return result;
}

} // namespace tessera::benchmark
