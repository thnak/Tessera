// SPDX-License-Identifier: MIT
//
// Chart node damage EQUIVALENCE on the deterministic CPU canvas (issue #183): "dirty-only and
// full-redraw output are equivalent".
//
// The first frame paints the whole chart (background + every retained box) into a persistent
// canvas, which stands in for what is currently on screen. One sample is then appended, the chart
// reports its damage, a redraw plan is built from it through the shared accumulator/policy, and the
// chart is repainted through paint::ClippedPaintSink into that same canvas - so only the damaged
// band can change. The result is compared byte for byte with a fresh full redraw of the updated
// chart into a blank canvas.
//
// Two guards keep the comparison from being vacuous: a few expected pixels must actually hold the
// series color, and a deliberately undersized region must be shown to leave stale pixels (so the
// equality above is not passing because everything repaints anyway).

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/chart/chart_render.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::chart::ChartDamage;
using tessera::ui::chart::ChartNode;
using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesStyle;
using tessera::ui::paint::ClippedPaintSink;
using tessera::ui::paint::PaintSink;

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 240;
constexpr Rect kPlot{16.0f, 16.0f, 288.0f, 200.0f};
// A pinned window, so an append moves one segment instead of rescaling the run. Capacity leaves
// room to append into: a full ring evicts and renumbers, which correctly damages the whole plot.
constexpr std::size_t kSamples = 40;
constexpr std::size_t kCapacity = 80;

bool expect(bool condition, const char* case_name, const char* message) {
    if (!condition) {
        std::cerr << "chart_node_equivalence[" << case_name << "]: " << message << "\n";
    }
    return condition;
}

// Solid-rect sink over the CPU canvas. The chart emits only axis-aligned opaque boxes, so the
// mapping is exact (no radius, text, vector, image or backdrop is involved).
class CpuSink final : public PaintSink {
public:
    explicit CpuSink(CpuCanvas& canvas)
        : canvas_(&canvas) {}

    void fill_rect(const Rect& rect, const Color& color) override {
        canvas_->fill_rect(rect, color);
    }

    // The second pure virtual on PaintSink; the chart emits only plain boxes, so a rounded one can
    // fall back to its bounds exactly like the documented CPU reference does.
    void fill_rounded_rect(const Rect& rect, const Color& color, float /*radius*/) override {
        canvas_->fill_rect(rect, color);
    }

private:
    CpuCanvas* canvas_ = nullptr;
};

ChartNode make_chart() {
    ChartNode chart(1, kCapacity, kPlot);
    chart.set_style(0, ChartSeriesStyle{Color{0.22f, 0.74f, 0.97f, 1.0f}, 3.0f, 0.0f});
    // A pinned window: an append then moves one segment rather than rescaling the whole run (an
    // auto-fit window is correct but always whole-plot damage, so it cannot show a partial
    // repaint).
    chart.set_time_range(tessera::ui::chart::ChartTimeRange{0.0, 80.0});
    chart.set_value_range(tessera::ui::chart::ChartValueRange{0.0f, 100.0f});
    for (std::size_t index = 0; index < kSamples; ++index) {
        // A zig-zag: consecutive samples alternate high/low, so every segment is a steep diagonal
        // and one append repaints one visible stripe rather than a flat run.
        const float value = (index % 2 == 0) ? 10.0f : 90.0f;
        chart.append(0, ChartSample{static_cast<double>(index), value});
    }
    chart.rebuild();
    return chart;
}

void paint_full(const ChartNode& chart, CpuCanvas& canvas) {
    CpuSink sink(canvas);
    canvas.fill_rect(Rect{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)},
                     Color{0.04f, 0.07f, 0.13f, 1.0f});
    canvas.fill_rect(kPlot, Color{0.06f, 0.10f, 0.16f, 1.0f});
    tessera::ui::chart::paint_chart(chart, sink, 1.0f);
}

void paint_damaged(const ChartNode& chart, CpuCanvas& canvas, const RedrawPlan& plan) {
    CpuSink sink(canvas);
    ClippedPaintSink clipped(sink, plan.scissors);
    tessera::ui::chart::paint_chart(chart, clipped, 1.0f);
}

std::size_t count_mismatches(const CpuCanvas& lhs, const CpuCanvas& rhs) {
    const auto left = lhs.pixels();
    const auto right = rhs.pixels();
    if (left.size() != right.size()) {
        return std::max(left.size(), right.size());
    }
    std::size_t mismatched = 0;
    for (std::size_t index = 0; index < left.size(); index += 4) {
        if (left[index] != right[index] || left[index + 1] != right[index + 1] ||
            left[index + 2] != right[index + 2] || left[index + 3] != right[index + 3]) {
            ++mismatched;
        }
    }
    return mismatched;
}

// True when the pixel is the series color, i.e. the chart really painted there.
bool is_series_pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    const auto pixels = canvas.pixels();
    // The CPU sink rounds each channel to nearest; allow +-2 for that rounding.
    const auto near = [](std::uint8_t actual, float expected) {
        const int target = static_cast<int>(expected * 255.0f + 0.5f);
        return actual >= target - 2 && actual <= target + 2;
    };
    return near(pixels[index], 0.22f) && near(pixels[index + 1], 0.74f) &&
           near(pixels[index + 2], 0.97f) && pixels[index + 3] == 255;
}

// Counts pixels inside a rectangle that hold the series color, so a case can prove its region
// actually covers chart content.
std::size_t series_pixels_in(const CpuCanvas& canvas, const Rect& rect) {
    std::size_t count = 0;
    const auto x0 = static_cast<std::uint32_t>(std::max(0.0f, rect.x));
    const auto y0 = static_cast<std::uint32_t>(std::max(0.0f, rect.y));
    const auto x1 = static_cast<std::uint32_t>(std::min(static_cast<float>(kWidth), rect.right()));
    const auto y1 =
        static_cast<std::uint32_t>(std::min(static_cast<float>(kHeight), rect.bottom()));
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            if (is_series_pixel(canvas, x, y)) {
                ++count;
            }
        }
    }
    return count;
}

// The acceptance case: one appended sample, a plan built from the chart's own damage,
// byte-identical output to a full redraw.
bool check_append_equivalence() {
    constexpr const char* kCase = "append";
    ChartNode chart = make_chart();

    CpuCanvas canvas(kWidth, kHeight);
    paint_full(chart, canvas);

    // A second series' worth of motion: one append, one changed segment.
    if (!expect(chart.append(0, ChartSample{static_cast<double>(kSamples), 35.0f}), kCase,
                "append accepted")) {
        return false;
    }
    const ChartDamage& damage = chart.rebuild();
    if (!expect(damage.size() > 0, kCase, "the append reported damage") ||
        !expect(!damage.full(), kCase, "the append did not overflow")) {
        return false;
    }

    DamageAccumulator accumulator(DamageConfig{Extent2D{kWidth, kHeight}, 16});
    for (std::size_t index = 0; index < damage.size(); ++index) {
        accumulator.add(damage[index]);
    }
    const RedrawPlan plan = make_redraw_plan(accumulator);
    if (!expect(!plan.full_redraw, kCase, "the plan is partial") ||
        !expect(plan.scissors.size() >= 1, kCase, "the plan has a scissor")) {
        return false;
    }

    paint_damaged(chart, canvas, plan);

    CpuCanvas expected(kWidth, kHeight);
    paint_full(chart, expected);

    if (!expect(count_mismatches(canvas, expected) == 0, kCase,
                "dirty-only pixels differ from a full redraw")) {
        std::cerr << "  mismatched=" << count_mismatches(canvas, expected) << "\n";
        return false;
    }
    // Guard against a blank-vs-blank pass: the plot must contain series pixels, and the damaged
    // region must contain the stripe that actually changed.
    const std::size_t plot_pixels = series_pixels_in(expected, kPlot);
    if (!expect(plot_pixels > 1000, kCase, "the plot contains the painted series")) {
        std::cerr << "  series pixels=" << plot_pixels << "\n";
        return false;
    }
    std::size_t damaged_series = 0;
    for (const auto& scissor : plan.scissors) {
        damaged_series += series_pixels_in(
            expected, Rect{static_cast<float>(scissor.x), static_cast<float>(scissor.y),
                           static_cast<float>(scissor.width), static_cast<float>(scissor.height)});
    }
    if (!expect(damaged_series > 0, kCase, "the damaged region contains painted series pixels")) {
        return false;
    }
    return true;
}

// The comparison is not vacuous: an undersized region must leave the canvas stale, which is exactly
// why a partial redraw has to cover the chart's reported damage.
bool check_undersized_region_leaves_stale_pixels() {
    constexpr const char* kCase = "undersized";
    ChartNode chart = make_chart();

    CpuCanvas canvas(kWidth, kHeight);
    paint_full(chart, canvas);

    if (!expect(chart.append(0, ChartSample{static_cast<double>(kSamples), 35.0f}), kCase,
                "append accepted")) {
        return false;
    }
    chart.rebuild();

    // One pixel in the plot's empty corner: far from the new segment.
    RedrawPlan plan;
    plan.scissors.push_back(tessera::rendering::rhi::Scissor{
        static_cast<std::int32_t>(kPlot.x) + 1, static_cast<std::int32_t>(kPlot.y) + 1, 2, 2});
    paint_damaged(chart, canvas, plan);

    CpuCanvas full(kWidth, kHeight);
    paint_full(chart, full);
    if (!expect(count_mismatches(canvas, full) > 0, kCase,
                "a region smaller than the damage leaves stale pixels")) {
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!check_append_equivalence() || !check_undersized_region_leaves_stale_pixels()) {
        return 1;
    }
    std::cout << "chart_node_equivalence: ok\n";
    return 0;
}
