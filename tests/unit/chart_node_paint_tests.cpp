// SPDX-License-Identifier: MIT
//
// Chart node painting and the hybrid immediate hook (issue #183,
// docs/specs/chart-node-v1.md sections 6 and 7), plus the `main` of the `chart_node` test binary.
//
// What this covers: the retained emission (every box, in series order, with the series color),
// opacity folding and the fully-transparent short-circuit, emission determinism, and the immediate
// escape - that it is off by default, runs once after the retained pass, receives the sink, the
// resolved plot and the effective opacity, and stops when cleared.
//
// The zero-allocation windows for the same per-frame path live in chart_node_allocation.cpp, and
// the geometry/damage/invalidation matrix in chart_node_tests.cpp.

#include "tessera/ui/chart/chart_node.hpp"
#include "tessera/ui/chart/chart_render.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace chart_node_test {

// Defined in chart_node_tests.cpp.
[[nodiscard]] bool geometry_and_damage();
// Defined in chart_node_allocation.cpp; true when every zero-allocation window held.
[[nodiscard]] bool allocation_free_after_construction();

} // namespace chart_node_test

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::ui::chart::ChartNode;
using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesStyle;
using tessera::ui::paint::PaintSink;

constexpr rhi::Rect kPlot{10.0f, 20.0f, 200.0f, 100.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "chart_node: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float epsilon = 1.0e-4f) {
    return (lhs > rhs ? lhs - rhs : rhs - lhs) <= epsilon;
}

// Records every fill the chart emits, in order, so emission order and counts are assertable.
class RecordingSink final : public PaintSink {
public:
    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }

    // The second pure virtual on PaintSink; the chart never emits a rounded box.
    void fill_rounded_rect(const rhi::Rect& /*rect*/, const rhi::Color& /*color*/,
                           float /*radius*/) override {}

    std::vector<rhi::Rect> rects;
    std::vector<rhi::Color> colors;
};

// ---- painting ----

bool check_paint_emission() {
    ChartNode chart(2, 8, kPlot);
    chart.set_style(0, ChartSeriesStyle{rhi::rgba8(255, 0, 0), 2.0f, 0.0f});
    chart.set_style(1, ChartSeriesStyle{rhi::rgba8(0, 0, 255), 2.0f, 4.0f});
    for (std::size_t index = 0; index < 4; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), 1.0f}), "seed append");
        expect(chart.append(1, ChartSample{static_cast<double>(index), 2.0f}), "seed append");
    }
    chart.rebuild();

    // Series 0: 3 segments, no markers. Series 1: 3 segments + 4 markers.
    RecordingSink sink;
    tessera::ui::chart::paint_chart(chart, sink, 1.0f);
    bool ok = expect(chart.retained_segments() == 6, "both series retain their segments") &&
              expect(chart.retained_markers() == 4, "only the marker series counts markers") &&
              expect(sink.rects.size() == 3 + 3 + 4, "every retained box is emitted") &&
              expect(near(sink.colors[0].r, 1.0f), "series 0 paints its color") &&
              expect(near(sink.colors[3].b, 1.0f), "series 1 paints its color after series 0");

    // Opacity folds into alpha, and a fully transparent paint emits nothing at all.
    RecordingSink translucent;
    tessera::ui::chart::paint_chart(chart, translucent, 0.5f);
    ok = expect(translucent.rects.size() == sink.rects.size(), "opacity does not drop boxes") &&
         expect(near(translucent.colors[0].a, 0.5f), "opacity folds into alpha") && ok;
    RecordingSink invisible;
    tessera::ui::chart::paint_chart(chart, invisible, 0.0f);
    ok = expect(invisible.rects.empty(), "a fully transparent chart emits nothing") && ok;

    // Emission order is deterministic: the same call twice gives the same stream.
    RecordingSink again;
    tessera::ui::chart::paint_chart(chart, again, 1.0f);
    ok = expect(again.rects.size() == sink.rects.size(), "the emitted count is stable") && ok;
    for (std::size_t index = 0; index < again.rects.size(); ++index) {
        ok = expect(near(again.rects[index].x, sink.rects[index].x) &&
                        near(again.rects[index].y, sink.rects[index].y),
                    "the emitted order is stable") &&
             ok;
    }

    // An empty chart emits nothing.
    ChartNode empty(1, 8, kPlot);
    empty.rebuild();
    RecordingSink nothing;
    tessera::ui::chart::paint_chart(empty, nothing, 1.0f);
    ok = expect(nothing.rects.empty(), "an empty chart emits nothing") && ok;
    return ok;
}

// ---- immediate hook ----

struct ImmediateProbe {
    std::size_t calls = 0;
    float opacity = 0.0f;
    rhi::Rect plot{};
    bool has_sink = false;
};

void immediate_hook(void* context, const tessera::ui::chart::ChartImmediateDraw& draw) {
    auto* probe = static_cast<ImmediateProbe*>(context);
    ++probe->calls;
    probe->opacity = draw.opacity;
    probe->has_sink = draw.sink != nullptr;
    if (draw.plot != nullptr) {
        probe->plot = draw.plot->rect;
    }
    if (draw.sink != nullptr) {
        draw.sink->fill_rect(rhi::Rect{0.0f, 0.0f, 2.0f, 2.0f}, rhi::rgba8(255, 255, 255));
    }
}

// The hybrid contract: the retained pass runs first, then the immediate hook, and the hook sees the
// resolved plot and the effective opacity.
bool check_immediate_hook() {
    ChartNode chart(1, 8, kPlot);
    chart.set_style(0, ChartSeriesStyle{rhi::rgba8(56, 189, 248), 2.0f, 0.0f});
    for (std::size_t index = 0; index < 4; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), 1.0f}), "seed append");
    }
    chart.rebuild();

    ImmediateProbe probe;
    bool ok = expect(!chart.has_immediate(), "no hook is installed by default");
    RecordingSink without;
    tessera::ui::chart::paint_chart(chart, without, 1.0f, chart.immediate(),
                                    chart.immediate_context());
    ok = expect(probe.calls == 0, "the hook did not run") && ok;

    chart.set_immediate(&immediate_hook, &probe);
    ok = expect(chart.has_immediate(), "the hook is installed") && ok;
    RecordingSink with;
    tessera::ui::chart::paint_chart(chart, with, 0.25f, chart.immediate(),
                                    chart.immediate_context());
    ok = expect(probe.calls == 1, "the hook ran once") &&
         expect(probe.has_sink, "the hook received the sink") &&
         expect(near(probe.opacity, 0.25f), "the hook received the effective opacity") &&
         expect(near(probe.plot.width, kPlot.width), "the hook received the resolved plot") &&
         expect(with.rects.size() == without.rects.size() + 1,
                "the hook's own draw is appended after the retained pass") &&
         ok;

    // The retained boxes precede the hook's rect, so an annotation draws over the plot.
    ok = expect(near(with.rects.back().width, 2.0f), "the hook drew last") && ok;

    // The retained pass through the outer overload is identical to the direct call.
    RecordingSink direct;
    tessera::ui::chart::paint_chart(static_cast<const ChartNode&>(chart), direct, 0.25f);
    ok = expect(direct.rects.size() == without.rects.size(),
                "the immediate hook does not change the retained stream") &&
         ok;

    // Clearing the hook stops it.
    chart.set_immediate(nullptr, nullptr);
    ok = expect(!chart.has_immediate(), "the hook can be cleared") && ok;
    tessera::ui::chart::paint_chart(chart, with, 1.0f, chart.immediate(),
                                    chart.immediate_context());
    ok = expect(probe.calls == 1, "a cleared hook does not run again") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = chart_node_test::geometry_and_damage() && ok;
    ok = check_paint_emission() && ok;
    ok = check_immediate_hook() && ok;
    ok = chart_node_test::allocation_free_after_construction() && ok;
    if (!ok) {
        std::cerr << "chart_node: FAILED\n";
        return 1;
    }
    std::cout << "chart_node: ok\n";
    return 0;
}
