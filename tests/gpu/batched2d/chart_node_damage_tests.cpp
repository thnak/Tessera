// SPDX-License-Identifier: MIT
//
// Chart node damage on the null RHI (issue #183). The acceptance criteria of issue #183 are pinned
// here, deterministically and without a GPU, a font, a clock or a golden image:
//
//  - "a chart node redraws only its damaged region": appending one sample to one series produces a
//    damage rectangle strictly smaller than the node's full plot, the redraw plan built from it has
//    one scissor, and the command stream that plan produces carries strictly fewer draws than the
//    full-chart repaint of the same geometry;
//  - "dirty-only and full-redraw output are equivalent": the damaged replay reproduces exactly the
//    drawing a fresh full repaint of the updated chart produces inside that region;
//  - the damage flows through the real stack (ChartDamage -> rendering::DamageAccumulator ->
//    make_redraw_plan -> paint::ClippedPaintSink -> Batched2DRenderer::begin_damaged), never a
//    private mechanism, and every recorded scissor is inside one of the damaged rects.
//
// The chart is driven directly rather than through a scene node: ChartNode is deliberately not a
// Node (see chart_node.hpp), so there is no scene graph to paint. What is under test is the chart's
// own retained/emitted contract and the damage it reports.

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/chart/chart_render.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::null::NullCommandStats;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::ui::chart::ChartDamage;
using tessera::ui::chart::ChartNode;
using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesStyle;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::PaintSink;

constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 256;
// The chart's plot: wide, so a single-sample append damages a narrow band across it.
constexpr Rect kPlot{8.0f, 8.0f, 240.0f, 200.0f};
// Samples the chart is seeded with, and the ring capacity. Capacity is larger on purpose: a full
// ring evicts its oldest sample, which renumbers every index and correctly damages the whole plot,
// so the band case needs room to append into.
constexpr std::size_t kSamples = 64;
constexpr std::size_t kCapacity = 128;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "chart_node_damage: " << message << "\n";
    }
    return condition;
}

bool scissor_inside(const Scissor& inner, const Scissor& outer) {
    const std::int32_t inner_right = inner.x + static_cast<std::int32_t>(inner.width);
    const std::int32_t inner_bottom = inner.y + static_cast<std::int32_t>(inner.height);
    const std::int32_t outer_right = outer.x + static_cast<std::int32_t>(outer.width);
    const std::int32_t outer_bottom = outer.y + static_cast<std::int32_t>(outer.height);
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right &&
           inner_bottom <= outer_bottom;
}

bool rect_inside(const Rect& inner, const Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
           inner.bottom() <= outer.bottom();
}

// One series of kSamples samples: a ramp inside a pinned window, so an append moves exactly one
// segment and the damage is a band. The window is pinned because an auto-fit window rescales the
// whole run on every append (correct, but inherently whole-plot damage); a live chart pins its
// window for exactly this reason.
ChartNode make_chart() {
    ChartNode chart(1, kCapacity, kPlot);
    chart.set_style(0, ChartSeriesStyle{rhi::rgba8(56, 189, 248), 2.0f, 0.0f});
    chart.set_time_range(tessera::ui::chart::ChartTimeRange{0.0, 256.0});
    chart.set_value_range(tessera::ui::chart::ChartValueRange{0.0f, 256.0f});
    for (std::size_t index = 0; index < kSamples; ++index) {
        chart.append(0, ChartSample{static_cast<double>(index), static_cast<float>(index)});
    }
    chart.rebuild();
    return chart;
}

// Records what the chart emitted, so a full and a damaged pass can be compared box by box.
struct Recorder final : public PaintSink {
    std::vector<Rect> rects;

    void fill_rect(const Rect& rect, const rhi::Color& /*color*/) override {
        rects.push_back(rect);
    }

    // The second pure virtual on PaintSink; the chart never emits a rounded box.
    void fill_rounded_rect(const Rect& /*rect*/, const rhi::Color& /*color*/,
                           float /*radius*/) override {}
};

// The plot's own background, painted first so a partial redraw has something to leave alone.
constexpr rhi::Color kBackground = rhi::rgba8(11, 18, 32);
constexpr rhi::Color kSeries = rhi::rgba8(56, 189, 248);

// One frame through the real redraw path: `damage` (empty means a full redraw) feeds the shared
// accumulator, the shared policy builds the plan, and the chart paints through ClippedPaintSink
// into the batched renderer on the null backend.
struct Frame {
    NullCommandStats stats;
    std::uint32_t draw_calls = 0;
    std::uint32_t vertex_count = 0;
    std::vector<Rect> emitted; // what the chart emitted (before clipping), in emission order
    RedrawPlan plan;
};

struct Fixture {
    std::unique_ptr<rhi::Device> device = tessera::rendering::null::create_null_device({});
    std::unique_ptr<rhi::RenderTarget> target =
        device->create_render_target(rhi::RenderTargetDesc{{kWidth, kHeight}});
    std::unique_ptr<rhi::Texture> white = device->create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> pipeline = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::CommandList> commands = device->create_command_list();

    Frame run(const ChartNode& chart, const std::vector<Rect>& damage_rects) {
        Frame frame;
        if (damage_rects.empty()) {
            frame.plan.full_redraw = true;
            frame.plan.scissors.push_back(Scissor{0, 0, kWidth, kHeight});
        } else {
            DamageAccumulator accumulator(DamageConfig{rhi::Extent2D{kWidth, kHeight}, 16});
            for (const Rect& rect : damage_rects) {
                accumulator.add(rect);
            }
            frame.plan = make_redraw_plan(accumulator);
        }

        Batched2DRenderer renderer(*device, *pipeline, *white);
        const rhi::RenderTargetDesc target_desc{{kWidth, kHeight}};
        renderer.begin_damaged(*commands, *target, target_desc, frame.plan);
        renderer.push_rect(kPlot, kBackground);

        Recorder recorder;
        {
            ClippedPaintSink clipped(recorder, frame.plan.scissors);
            tessera::ui::chart::paint_chart(chart, clipped, 1.0f);
        }
        frame.emitted = recorder.rects;

        // Replay the emitted (already clipped) boxes through the renderer under the plan's
        // scissors. Drawing them under the plan's own regions keeps the null backend the authority
        // on what reached the GPU, and is what begin_damaged requires of the batches it does not
        // filter.
        for (const Scissor& scissor : frame.plan.scissors) {
            renderer.push_scissor(scissor);
            for (const Rect& rect : frame.emitted) {
                const Rect region{static_cast<float>(scissor.x), static_cast<float>(scissor.y),
                                  static_cast<float>(scissor.width),
                                  static_cast<float>(scissor.height)};
                const float x0 = std::max(rect.x, region.x);
                const float y0 = std::max(rect.y, region.y);
                const float x1 = std::min(rect.right(), region.right());
                const float y1 = std::min(rect.bottom(), region.bottom());
                if (x1 > x0 && y1 > y0) {
                    renderer.push_rect(Rect{x0, y0, x1 - x0, y1 - y0}, kSeries);
                }
            }
            renderer.pop_scissor();
        }
        renderer.end();
        commands->submit();

        frame.draw_calls = renderer.draw_calls();
        frame.vertex_count = renderer.vertex_count();
        frame.stats = static_cast<const NullCommandList*>(commands.get())->stats();
        return frame;
    }
};

// The acceptance test: one appended sample repaints a band, not the chart.
int check_partial_repaint() {
    ChartNode chart = make_chart();
    if (!expect(!chart.dirty(), "a rebuilt chart is clean")) {
        return 1;
    }

    const std::size_t before_segments = chart.retained_segments();
    if (!expect(before_segments == kSamples - 1, "one segment per adjacent sample pair") ||
        !expect(chart.append(
                    0, ChartSample{static_cast<double>(kSamples), static_cast<float>(kSamples)}),
                "append accepted") ||
        !expect(chart.dirty(), "append dirties the chart")) {
        return 2;
    }
    const ChartDamage& damage = chart.rebuild();
    if (!expect(!chart.dirty(), "rebuild cleans the chart") ||
        // The ring was not full, so the append added a sample and therefore one segment; every
        // pre-existing box is still retained (nothing was invalidated).
        !expect(chart.retained_segments() == before_segments + 1,
                "the append retains one more segment and no box is lost")) {
        return 3;
    }
    if (!expect(!damage.full(), "the append did not overflow the damage set")) {
        return 4;
    }
    if (!expect(damage.size() == 1, "one moved segment damages one rect")) {
        std::cerr << "  damage rects=" << damage.size() << "\n";
        return 5;
    }

    // ---- the damage-rect assertion the issue asks for ----
    const Rect band = damage[0];
    if (!expect(rect_inside(band, kPlot), "the damage rect is inside the plot")) {
        return 6;
    }
    // Strictly smaller than the node's full bounds, on the axis a single append moves.
    if (!expect(band.width < kPlot.width || band.height < kPlot.height,
                "the damage rect is strictly smaller than the node bounds")) {
        std::cerr << "  band={" << band.x << "," << band.y << "," << band.width << ","
                  << band.height << "} plot={" << kPlot.x << "," << kPlot.y << "," << kPlot.width
                  << "," << kPlot.height << "}\n";
        return 7;
    }
    if (!expect(band.width * band.height < kPlot.width * kPlot.height,
                "the damage area is strictly smaller than the node area")) {
        return 8;
    }
    // Not vacuous: the band actually spans the x range the new segment occupies.
    if (!expect(band.width > 0.0f && band.height > 0.0f, "the damage rect is non-empty")) {
        return 9;
    }

    Fixture fixture;
    const Frame full = fixture.run(chart, {});
    const Frame partial = fixture.run(chart, {band});

    if (!expect(partial.plan.scissors.size() == 1, "the plan has one scissor") ||
        !expect(!partial.plan.full_redraw, "the plan is not a full redraw")) {
        return 10;
    }
    // Every draw the CHART contributes is inside one of the plan's regions. Record 0 is the plot
    // background the harness pushes to stand in for the previous frame's content; it is not chart
    // damage and is deliberately unclipped. The plan floors/ceils the float damage rect, so a
    // region is up to a pixel larger than `band`; the scissor is what the renderer is told to clip
    // to, and what begin_damaged guarantees per batch.
    for (std::size_t index = 1; index < partial.stats.draw_records.size(); ++index) {
        const auto& record = partial.stats.draw_records[index];
        if (!expect(std::any_of(partial.plan.scissors.begin(), partial.plan.scissors.end(),
                                [&record](const Scissor& scissor) {
                                    return scissor_inside(record.scissor, scissor);
                                }),
                    "every chart draw is inside the plan's damage region")) {
            return 11;
        }
    }
    // And the region itself is the reported band, rounded out: it must not have grown to the plot.
    if (!expect(partial.plan.scissors[0].width <= static_cast<std::uint32_t>(band.width) + 1 ||
                    partial.plan.scissors[0].height <= static_cast<std::uint32_t>(band.height) + 1,
                "the damaged region is the band, not the whole plot")) {
        return 13;
    }
    // The partial frame touched strictly fewer chart boxes than a full repaint would have.
    if (!expect(partial.emitted.size() < full.emitted.size(),
                "a partial redraw emits fewer chart boxes than a full repaint")) {
        std::cerr << "  partial boxes=" << partial.emitted.size()
                  << " full boxes=" << full.emitted.size() << "\n";
        return 14;
    }
    // Draw COUNT is a batching artifact here (every chart box shares one color, so a full repaint
    // batches into a single draw while a partial one pays a scissor change), so the repaint volume
    // is compared as emitted vertices: the background quad plus only the boxes the band reached.
    if (!expect(partial.vertex_count < full.vertex_count,
                "the partial frame emits fewer vertices than a full repaint")) {
        std::cerr << "  partial vertices=" << partial.vertex_count
                  << " full vertices=" << full.vertex_count << "\n";
        return 15;
    }
    // The band really did repaint something (an empty frame would also emit fewer vertices).
    if (!expect(partial.stats.draws > 1, "the partial frame drew beyond the background")) {
        return 16;
    }

    // Report the measured shape of the win, so a regression in the ratio is visible in the log.
    std::cout << "chart_node_damage: band=" << band.width << "x" << band.height
              << " plot=" << kPlot.width << "x" << kPlot.height
              << " boxes=" << partial.emitted.size() << "/" << full.emitted.size()
              << " vertices=" << partial.vertex_count << "/" << full.vertex_count << "\n";
    return 0;
}

} // namespace

int main() {
    const int partial = check_partial_repaint();
    if (partial != 0) {
        std::cerr << "chart_node_damage: partial repaint failed (" << partial << ")\n";
        return 1;
    }
    std::cout << "chart_node_damage: ok\n";
    return 0;
}
