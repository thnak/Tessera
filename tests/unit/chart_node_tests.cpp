// SPDX-License-Identifier: MIT
//
// Chart node geometry, damage and invalidation (issue #183, docs/specs/chart-node-v1.md).
//
// This file covers everything decidable from samples and boxes alone: the domain mapping, the box
// construction the damage rule depends on, the damage set, the damage a rebuild reports, the
// rejection rules and the invalidation matrix. Painting, the immediate hook and the allocation
// windows live in chart_node_paint_tests.cpp; both are linked into the `chart_node` test binary.

#include "tessera/ui/chart/chart_geometry.hpp"
#include "tessera/ui/chart/chart_node.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::ui::chart::ChartDamage;
using tessera::ui::chart::ChartNode;
using tessera::ui::chart::ChartPlot;
using tessera::ui::chart::ChartRange;
using tessera::ui::chart::ChartSample;
using tessera::ui::chart::ChartSeriesGeometry;
using tessera::ui::chart::ChartSeriesStyle;
using tessera::ui::chart::ChartTimeRange;
using tessera::ui::chart::ChartValueRange;
using tessera::ui::chart::empty_rect;

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

ChartSeriesStyle style_of(float thickness, float marker_size = 0.0f) {
    return ChartSeriesStyle{rhi::rgba8(56, 189, 248), thickness, marker_size};
}

// ---- geometry mapping ----

bool check_mapping() {
    ChartPlot plot{kPlot, ChartTimeRange{0.0, 10.0}, ChartValueRange{0.0f, 100.0f}};
    // Time maps left to right; value maps bottom to top (the y axis is inverted).
    bool ok =
        expect(near(plot.x_for(0.0), kPlot.x), "time minimum maps to the left edge") &&
        expect(near(plot.x_for(10.0), kPlot.right()), "time maximum maps to the right edge") &&
        expect(near(plot.x_for(5.0), kPlot.x + kPlot.width * 0.5f), "time midpoint") &&
        expect(near(plot.y_for(0.0f), kPlot.bottom()), "value minimum maps to the bottom") &&
        expect(near(plot.y_for(100.0f), kPlot.y), "value maximum maps to the top") &&
        expect(near(plot.y_for(50.0f), kPlot.y + kPlot.height * 0.5f), "value midpoint");
    // Out-of-domain samples clamp to the edges instead of escaping the plot.
    ok = expect(near(plot.x_for(-5.0), kPlot.x), "a sample before the window clamps left") &&
         expect(near(plot.y_for(1000.0f), kPlot.y), "a value above the window clamps to the top") &&
         expect(near(plot.x_for(50.0), kPlot.right()), "a sample after the window clamps right");
    // A degenerate domain does not divide by zero.
    const ChartPlot flat{kPlot, ChartTimeRange{}, ChartValueRange{}};
    ok = expect(near(flat.x_for(3.0), kPlot.x), "a flat time domain maps to the left edge") &&
         expect(near(flat.y_for(3.0f), kPlot.bottom()), "a flat value domain maps to the bottom");
    return ok;
}

// Segment and marker boxes are the stroked/centred boxes the damage rule depends on.
bool check_boxes() {
    // A horizontal 4px stroke from (10,10) to (30,10): 20 wide, 4 tall, inflated by 2 each side.
    const rhi::Rect horizontal = tessera::ui::chart::segment_box(10.0f, 10.0f, 30.0f, 10.0f, 4.0f);
    bool ok = expect(near(horizontal.x, 8.0f) && near(horizontal.y, 8.0f) &&
                         near(horizontal.width, 24.0f) && near(horizontal.height, 4.0f),
                     "a horizontal segment box is the stroke band");
    // A diagonal covers its whole bounding box (the documented V1 approximation).
    const rhi::Rect diagonal = tessera::ui::chart::segment_box(0.0f, 0.0f, 10.0f, 10.0f, 2.0f);
    ok = expect(near(diagonal.x, -1.0f) && near(diagonal.width, 12.0f) &&
                    near(diagonal.height, 12.0f),
                "a diagonal segment covers its bounding box") &&
         ok;
    // A zero thickness still yields the exact segment extent.
    const rhi::Rect hairline = tessera::ui::chart::segment_box(0.0f, 5.0f, 10.0f, 5.0f, 0.0f);
    ok = expect(near(hairline.height, 0.0f), "a zero-thickness segment is degenerate") && ok;
    // A marker is centred on its point.
    const rhi::Rect marker = tessera::ui::chart::marker_box(20.0f, 30.0f, 6.0f);
    ok = expect(near(marker.x, 17.0f) && near(marker.y, 27.0f) && near(marker.width, 6.0f),
                "a marker is centred on its point") &&
         ok;
    // Non-finite input collapses to an empty box rather than painting garbage.
    ok = expect(empty_rect(tessera::ui::chart::segment_box(std::nanf(""), 0.0f, 1.0f, 1.0f, 1.0f)),
                "a non-finite segment box is empty") &&
         expect(empty_rect(tessera::ui::chart::marker_box(0.0f, 0.0f, 0.0f)),
                "an unset marker box is empty") &&
         ok;
    return ok;
}

// ---- damage ----

// The damage set merges touching rects, keeps disjoint ones apart, and falls back to the whole
// fallback rectangle when it overflows - never under-damaging.
bool check_damage_set() {
    const rhi::Rect fallback{0.0f, 0.0f, 100.0f, 100.0f};
    ChartDamage damage;
    damage.add(rhi::Rect{10.0f, 10.0f, 10.0f, 10.0f}, fallback);
    bool ok = expect(damage.size() == 1 && !damage.full(), "one rect is stored");
    // A touching rect merges into the first.
    damage.add(rhi::Rect{20.0f, 10.0f, 10.0f, 10.0f}, fallback);
    ok = expect(damage.size() == 1, "touching rects merge") &&
         expect(near(damage[0].x, 10.0f) && near(damage[0].width, 20.0f), "the union is exact") &&
         ok;
    // A disjoint rect stays separate.
    damage.add(rhi::Rect{60.0f, 60.0f, 10.0f, 10.0f}, fallback);
    ok = expect(damage.size() == 2, "a disjoint rect is kept apart") && ok;
    // Empty rects are ignored.
    damage.add(rhi::Rect{}, fallback);
    ok = expect(damage.size() == 2, "an empty rect adds nothing") && ok;
    // Overflow: the set is replaced by the fallback and flagged.
    ChartDamage overflowing;
    for (int index = 0; index < 32; ++index) {
        overflowing.add(rhi::Rect{static_cast<float>(index) * 50.0f, 0.0f, 10.0f, 10.0f}, fallback);
    }
    ok = expect(overflowing.full(), "overflowing the set sets the fallback flag") &&
         expect(overflowing.size() == 1, "the fallback is the only rect") &&
         expect(near(overflowing[0].width, fallback.width), "the fallback is the plot") && ok;
    // clear() resets both.
    overflowing.clear();
    ok =
        expect(overflowing.size() == 0 && !overflowing.full(), "clear resets the damage set") && ok;
    return ok;
}

// A single append on a chart with room left rebuilds only the segment that changed, and the damage
// is that segment's band - strictly smaller than the plot. The window is pinned (a live chart's
// usual configuration): with an auto-fit window every append rescales the whole run, which is
// correct but inherently whole-plot damage, and check_autofit_is_whole_plot covers that case.
bool check_single_append_damage() {
    ChartNode chart(1, 32, kPlot);
    chart.set_style(0, style_of(2.0f));
    chart.set_time_range(ChartTimeRange{0.0, 40.0});
    chart.set_value_range(ChartValueRange{0.0f, 40.0f});
    for (std::size_t index = 0; index < 8; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), static_cast<float>(index)}),
               "seed append");
    }
    chart.rebuild();
    if (!expect(!chart.dirty(), "a rebuilt chart is clean")) {
        return false;
    }

    // One append: the new sample extends the ramp, so exactly one segment is rebuilt.
    if (!expect(chart.append(0, ChartSample{8.0, 8.0f}), "append accepted") ||
        !expect(chart.dirty(), "an append dirties the chart")) {
        return false;
    }
    const ChartDamage& damage = chart.rebuild();
    bool ok = expect(!chart.dirty(), "rebuild clears the dirty range") &&
              expect(damage.size() == 1, "one rebuilt segment damages one rect") &&
              expect(!damage.full(), "no overflow") &&
              expect(damage[0].width < kPlot.width && damage[0].height * 2.0f < kPlot.height,
                     "the damaged band is smaller than the plot");

    const ChartSeriesGeometry& geometry = chart.geometry(0);
    ok = expect(geometry.built_points == 9, "nine samples are retained") &&
         expect(chart.retained_segments() == 8, "eight segments for nine samples") && ok;
    // Sample 8 of a 0..40 window sits at 20% of the width, so the newest segment ends there.
    ok = expect(near(geometry.segments[7].right(), kPlot.x + kPlot.width * 0.2f + 1.0f),
                "the newest segment ends where sample 8 maps") &&
         ok;
    // The untouched segments did not change.
    for (std::size_t index = 0; index + 1 < 7; ++index) {
        ok =
            expect(!empty_rect(geometry.segments[index]), "an old segment is still retained") && ok;
    }
    return ok;
}

// An auto-fit window genuinely moves every sample when an append changes the domain, so the damage
// is the whole plot. This is over-damage by design, and it is why a live chart pins its window.
bool check_autofit_is_whole_plot() {
    ChartNode chart(1, 32, kPlot);
    chart.set_style(0, style_of(2.0f));
    for (std::size_t index = 0; index < 8; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), static_cast<float>(index)}),
               "seed append");
    }
    chart.rebuild();
    if (!expect(chart.append(0, ChartSample{8.0, 8.0f}), "append accepted")) {
        return false;
    }
    const ChartDamage& damage = chart.rebuild();
    // Sample 8 is now the newest, so the window is 0..8 and every earlier sample moves.
    bool ok = expect(damage.size() >= 1, "an auto-fit append damages") &&
              expect(damage.full() ||
                         damage[0].width * damage[0].height >= kPlot.width * kPlot.height * 0.9f,
                     "an auto-fit append damages the whole plot");
    // ...but never less than the plot, and never absurdly more.
    if (!damage.full()) {
        ok = expect(damage[0].x <= kPlot.x && damage[0].right() >= kPlot.right(),
                    "the whole-plot damage covers the plot") &&
             ok;
    }
    return ok;
}

// A full ring evicts its oldest sample, which renumbers every index: the whole run is rebuilt and
// the damage covers the plot rather than a band. Over-damage, never under-damage.
bool check_eviction_rebuilds_whole() {
    ChartNode chart(1, 8, kPlot);
    chart.set_style(0, style_of(2.0f));
    for (std::size_t index = 0; index < 8; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), static_cast<float>(index)}),
               "seed append");
    }
    chart.rebuild();
    if (!expect(chart.series(0).full(), "the ring is full")) {
        return false;
    }
    if (!expect(chart.append(0, ChartSample{8.0, 8.0f}), "append accepted")) {
        return false;
    }
    const ChartDamage& damage = chart.rebuild();
    return expect(damage.size() >= 1, "eviction damages something") &&
           expect(damage.full() ||
                      damage[0].width * damage[0].height >= kPlot.width * kPlot.height * 0.9f,
                  "eviction damages (nearly) the whole plot");
}

// A rebuild with nothing dirty does no work and reports no damage.
bool check_clean_rebuild_is_noop() {
    ChartNode chart(1, 8, kPlot);
    chart.set_style(0, style_of(2.0f));
    expect(chart.append(0, ChartSample{0.0, 0.0f}), "seed append");
    expect(chart.append(0, ChartSample{1.0, 1.0f}), "seed append");
    const ChartDamage& first = chart.rebuild();
    if (!expect(first.size() > 0, "the first rebuild damages")) {
        return false;
    }
    const ChartDamage& second = chart.rebuild();
    return expect(second.size() == 0 && !second.full(), "a clean rebuild reports no damage") &&
           expect(!chart.dirty(), "a clean rebuild leaves the chart clean");
}

// ---- rejection ----

bool check_rejections() {
    ChartNode chart(2, 4, kPlot);
    chart.set_style(0, style_of(2.0f));
    expect(chart.append(0, ChartSample{0.0, 1.0f}), "seed append");
    chart.rebuild();

    // A series index out of range, a non-finite value and a non-increasing timestamp are all
    // rejected without touching the chart.
    bool ok =
        expect(!chart.append(2, ChartSample{1.0, 1.0f}), "an out-of-range series is rejected") &&
        expect(!chart.append(0, ChartSample{1.0, std::nanf("")}),
               "a non-finite value is rejected") &&
        expect(!chart.append(0, ChartSample{0.0, 1.0f}), "a repeat timestamp is rejected") &&
        expect(!chart.append(0, ChartSample{-1.0, 1.0f}), "a backward timestamp is rejected") &&
        expect(chart.series(0).size() == 1, "a rejected append does not grow the ring");
    ok = expect(!chart.dirty(), "a rejected append does not dirty the chart") && ok;
    // The rejected appends left no damage behind.
    ok = expect(chart.rebuild().size() == 0, "a rejected append reports no damage") && ok;

    // Construction rejects nonsense: zero series, zero capacity.
    bool threw = false;
    try {
        ChartNode invalid(0, 4, kPlot);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ok = expect(threw, "a zero series count is rejected") && ok;
    threw = false;
    try {
        ChartNode invalid(1, 0, kPlot);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ok = expect(threw, "a zero capacity is rejected") && ok;
    return ok;
}

// ---- ranges ----

bool check_ranges() {
    ChartNode chart(1, 8, kPlot);
    chart.set_style(0, style_of(0.0f));
    expect(chart.append(0, ChartSample{10.0, -5.0f}), "seed append");
    expect(chart.append(0, ChartSample{20.0, 15.0f}), "seed append");
    chart.rebuild();

    // Auto-fit resolves to the stored samples.
    bool ok = expect(chart.plot().time.min == 10.0 && chart.plot().time.max == 20.0,
                     "auto time fits the samples") &&
              expect(near(chart.plot().value.min, -5.0f) && near(chart.plot().value.max, 15.0f),
                     "auto value fits the samples");

    // An explicit range wins and is not overwritten by the next rebuild.
    chart.set_time_range(ChartTimeRange{0.0, 100.0});
    chart.set_value_range(ChartValueRange{0.0f, 50.0f});
    chart.rebuild();
    ok = expect(chart.plot().time.min == 0.0 && chart.plot().time.max == 100.0,
                "an explicit time range is kept") &&
         expect(near(chart.plot().value.max, 50.0f), "an explicit value range is kept") && ok;
    // A new sample does not move an explicit window.
    expect(chart.append(0, ChartSample{30.0, 25.0f}), "append inside the explicit window");
    chart.rebuild();
    ok = expect(chart.plot().time.max == 100.0, "an append does not move an explicit window") && ok;

    // Returning to auto re-fits on the next rebuild.
    chart.set_auto_ranges();
    chart.rebuild();
    ok = expect(chart.plot().time.max == 30.0, "set_auto_ranges re-fits the window") && ok;

    // A disabled range value means auto (the `enabled()` contract).
    ok = expect(!ChartRange<double>{}.enabled(), "an empty range is disabled") &&
         expect(ChartTimeRange{5.0, 5.0}.enabled() == false, "a flat range is disabled") &&
         expect(ChartTimeRange{0.0, 1.0}.enabled(), "a real range is enabled") && ok;
    return ok;
}

// A plot or style change invalidates the whole retained run; a color change does not.
bool check_invalidation() {
    ChartNode chart(1, 8, kPlot);
    chart.set_style(0, style_of(2.0f));
    for (std::size_t index = 0; index < 4; ++index) {
        expect(chart.append(0, ChartSample{static_cast<double>(index), 1.0f}), "seed append");
    }
    chart.rebuild();

    // A color-only style change keeps the geometry (colors are read at emission time).
    chart.set_style(0, ChartSeriesStyle{rhi::rgba8(255, 0, 0), 2.0f, 0.0f});
    bool ok = expect(!chart.dirty(), "a color-only change keeps retained geometry");
    // A thickness change invalidates it.
    chart.set_style(0, style_of(6.0f));
    ok = expect(chart.dirty(), "a thickness change invalidates retained geometry") && ok;
    chart.rebuild();
    // A marker size change invalidates it.
    chart.set_style(0, style_of(6.0f, 4.0f));
    ok = expect(chart.dirty(), "a marker size change invalidates retained geometry") && ok;
    chart.rebuild();
    // A plot move invalidates it.
    chart.set_plot(rhi::Rect{0.0f, 0.0f, 100.0f, 50.0f});
    ok = expect(chart.dirty(), "a plot change invalidates retained geometry") && ok;
    chart.rebuild();
    ok = expect(!chart.dirty(), "the chart is clean after rebuilding") && ok;
    // Setting the same plot again is a no-op.
    chart.set_plot(rhi::Rect{0.0f, 0.0f, 100.0f, 50.0f});
    ok = expect(!chart.dirty(), "setting the same plot changes nothing") && ok;
    return ok;
}

// Series count and capacity are reported and stable; geometry storage is pre-sized.
bool check_shape() {
    ChartNode chart(3, 16, kPlot);
    bool ok =
        expect(chart.series_count() == 3, "the series count is as constructed") &&
        expect(chart.capacity() == 16, "the capacity is as constructed") &&
        expect(chart.geometry(0).segments.size() == 16, "geometry is pre-sized to the capacity") &&
        expect(chart.geometry(2).markers.size() == 16,
               "marker geometry is pre-sized to the capacity") &&
        expect(chart.retained_segments() == 0, "an empty chart retains no segments") &&
        expect(chart.retained_markers() == 0, "an empty chart retains no markers");
    // An empty chart with nothing dirty damages nothing.
    const ChartDamage& damage = chart.rebuild();
    ok = expect(damage.size() == 0, "an empty chart with nothing dirty damages nothing") && ok;
    return ok;
}

} // namespace

namespace chart_node_test {

bool geometry_and_damage() {
    bool ok = true;
    ok = check_mapping() && ok;
    ok = check_boxes() && ok;
    ok = check_damage_set() && ok;
    ok = check_single_append_damage() && ok;
    ok = check_autofit_is_whole_plot() && ok;
    ok = check_eviction_rebuilds_whole() && ok;
    ok = check_clean_rebuild_is_noop() && ok;
    ok = check_rejections() && ok;
    ok = check_ranges() && ok;
    ok = check_invalidation() && ok;
    ok = check_shape() && ok;
    return ok;
}

} // namespace chart_node_test
