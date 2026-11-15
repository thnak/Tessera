// SPDX-License-Identifier: MIT
//
// Hybrid chart node (issue #183). Normative contract: docs/specs/chart-node-v1.md; the sample
// storage it owns is docs/specs/chart-series-v1.md (issue #182).
//
// "Hybrid" is the two halves of the paint contract, and they are split across this header and
// chart_render.hpp:
//
//   * RETAINED - this header. One ChartSeriesRing per series plus the ChartSeriesGeometry built
//     from it (chart_geometry.hpp) and the damage a rebuild implies. Nothing here talks to a sink:
//     a chart's repaint cost is decided by its geometry and its damage, not by its draw calls.
//   * IMMEDIATE - chart_render.hpp's ChartImmediateFn hook, one plain function pointer plus a
//     caller-owned context, invoked after the retained pass with the same sink and resolved plot.
//     It exists because a chart inevitably grows annotations, threshold bands, brush selections and
//     real stroked polylines that no retained box set can express; giving that an escape hatch now
//     keeps the retained path free of speculative generality.
//
// The type is deliberately NOT part of ui::Node: Node is at its source-size baseline and a chart
// needs no new node capability (no layout input, no hit testing, no per-node style beyond what the
// owner already resolves). It borrows a world-space plot rectangle instead, in the same absolute
// device-pixel space PaintSink draws in, and the owner that already knows the node's box computes
// it. Damage is a caller-side concern for the same reason: rebuild() reports a bounded ChartDamage,
// and the owner feeds those rectangles to rendering::DamageAccumulator exactly like any other
// damage source, so a chart cannot invent a second redraw mechanism.
//
// The type is header-only for the same reason chart_series.hpp is: no ABI surface, no library
// target, and therefore no build-system change to adopt it.
//
// Steady-state allocation: zero. append()/rebuild() write into storage sized by the constructor;
// the only allocations happen there (the rings and the per-series geometry vectors).

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/chart/chart_geometry.hpp"
#include "tessera/ui/chart/chart_series.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace tessera::ui::paint {
class PaintSink;
} // namespace tessera::ui::paint

namespace tessera::ui::chart {

// Forward-declared so this header stays free of the paint stack: the immediate hook only carries a
// borrowed pointer to a sink, never a definition of one.
namespace paint = tessera::ui::paint;

// Per-series appearance. Retained V1 strokes are solid boxes (chart-node-v1 section 4), so a series
// needs only a color, a stroke width and an optional marker size.
struct ChartSeriesStyle {
    rhi::Color color{0.35f, 0.75f, 0.98f, 1.0f};
    float thickness = 2.0f;
    // Marker edge length in device pixels; 0 disables the sample markers (and their geometry).
    float marker_size = 0.0f;
};

// The immediate-mode escape (see chart_render.hpp for the emission side). `sink` and `plot` are
// borrowed for the duration of the call; the callback must not retain them.
struct ChartImmediateDraw {
    tessera::ui::paint::PaintSink* sink = nullptr;
    const ChartPlot* plot = nullptr;
    float opacity = 1.0f;
};
using ChartImmediateFn = void (*)(void* context, const ChartImmediateDraw& draw);

class ChartNode final {
public:
    // Allocates every buffer this chart will ever need: `series_count` rings of `capacity`
    // samples, the matching geometry, and the style slots. Throws std::invalid_argument for a zero
    // series count or a zero capacity (the ring's own rule), std::bad_alloc on allocation
    // failure. Nothing after construction allocates.
    ChartNode(std::size_t series_count, std::size_t capacity, const rhi::Rect& plot);

    ChartNode(const ChartNode&) = delete;
    ChartNode& operator=(const ChartNode&) = delete;
    ChartNode(ChartNode&&) = default;
    ChartNode& operator=(ChartNode&&) = default;

    [[nodiscard]] std::size_t series_count() const noexcept {
        return series_.size();
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return series_.empty() ? 0 : series_[0].capacity();
    }
    [[nodiscard]] ChartSeriesRing& series(std::size_t index) noexcept {
        return series_[index];
    }
    [[nodiscard]] const ChartSeriesRing& series(std::size_t index) const noexcept {
        return series_[index];
    }
    [[nodiscard]] const ChartSeriesStyle& style(std::size_t index) const noexcept {
        return styles_[index];
    }
    void set_style(std::size_t index, const ChartSeriesStyle& style) noexcept;

    // The plot rectangle (absolute device pixels) and the two data domains. A disabled range
    // (max <= min) auto-fits to the samples currently stored, which is the only mode that can react
    // to new data without the caller respecifying its window. Changing the rectangle or an explicit
    // range invalidates every retained segment; an auto-fit domain change is detected by rebuild().
    void set_plot(const rhi::Rect& plot) noexcept;
    [[nodiscard]] const ChartPlot& plot() const noexcept {
        return plot_;
    }
    void set_time_range(const ChartTimeRange& range) noexcept;
    void set_value_range(const ChartValueRange& range) noexcept;
    void set_auto_ranges() noexcept;

    // Appends one sample to `index` and marks the segments the append can have moved. Returns
    // false when the ring rejects the sample (non-finite or non-increasing timestamp), the index is
    // out of range, or the value is not finite; the chart is unchanged in every one of those cases.
    // Oldest-sample eviction invalidates the whole retained run, because every remaining sample
    // then shifts one logical index towards the left edge.
    bool append(std::size_t index, ChartSample sample) noexcept;

    // Resolves the domains, rebuilds the segments that changed, and returns the damage those
    // rebuilds imply: the union of the old and new box of every rebuilt segment and marker. An
    // append of one sample on a full ring therefore damages a band, not the whole chart. A call
    // with nothing dirty returns an empty ChartDamage after doing no work.
    const ChartDamage& rebuild() noexcept;
    [[nodiscard]] const ChartDamage& damage() const noexcept {
        return damage_;
    }
    [[nodiscard]] bool dirty() const noexcept;

    // Retained geometry of one series, as last resolved by rebuild().
    [[nodiscard]] const ChartSeriesGeometry& geometry(std::size_t index) const noexcept {
        return geometry_[index];
    }
    // Retained segment and marker counts over every series (the marker count counts only the
    // series that paint markers). Both are bounded by capacity().
    [[nodiscard]] std::size_t retained_segments() const noexcept;
    [[nodiscard]] std::size_t retained_markers() const noexcept;

    // The immediate-mode hook, or null. `context` is passed back verbatim and must outlive the
    // chart or be cleared first. The hook runs after the retained pass; its output is clipped by
    // whatever sink the caller paints through, so it needs no damage logic of its own.
    void set_immediate(ChartImmediateFn draw, void* context) noexcept {
        immediate_ = draw;
        immediate_context_ = context;
    }
    [[nodiscard]] ChartImmediateFn immediate() const noexcept {
        return immediate_;
    }
    [[nodiscard]] void* immediate_context() const noexcept {
        return immediate_context_;
    }
    [[nodiscard]] bool has_immediate() const noexcept {
        return immediate_ != nullptr;
    }

private:
    // Resolves an auto-fit domain from the samples; leaves an explicit one alone.
    void resolve_ranges() noexcept;
    void rebuild_series(std::size_t index, float thickness, float marker_size) noexcept;

    std::vector<ChartSeriesRing> series_;
    std::vector<ChartSeriesGeometry> geometry_;
    std::vector<ChartSeriesStyle> styles_;
    ChartPlot plot_{};
    // True while a domain is auto-fit; set_time_range()/set_value_range() clear the matching flag.
    bool auto_time_enabled_ = true;
    bool auto_value_enabled_ = true;
    ChartDamage damage_{};
    ChartImmediateFn immediate_ = nullptr;
    void* immediate_context_ = nullptr;
};

inline ChartNode::ChartNode(std::size_t series_count, std::size_t capacity, const rhi::Rect& plot) {
    if (series_count == 0) {
        throw std::invalid_argument("a chart node needs at least one series");
    }
    // reserve + emplace, not resize: ChartSeriesRing is move-only and sized by its constructor.
    series_.reserve(series_count);
    for (std::size_t index = 0; index < series_count; ++index) {
        series_.emplace_back(capacity);
    }
    geometry_.resize(series_count);
    for (ChartSeriesGeometry& entry : geometry_) {
        entry.reserve(capacity);
    }
    styles_.resize(series_count);
    plot_.rect = plot;
}

inline void ChartNode::set_style(std::size_t index, const ChartSeriesStyle& style) noexcept {
    if (index >= styles_.size()) {
        return;
    }
    const ChartSeriesStyle& previous = styles_[index];
    // Stroke width and marker size are baked into the retained boxes, so a change to either
    // invalidates them. A color change does not: colors are read at emission time.
    if (previous.thickness != style.thickness || previous.marker_size != style.marker_size) {
        geometry_[index].mark_all();
    }
    styles_[index] = style;
}

inline void ChartNode::set_plot(const rhi::Rect& plot) noexcept {
    if (plot_.rect.x == plot.x && plot_.rect.y == plot.y && plot_.rect.width == plot.width &&
        plot_.rect.height == plot.height) {
        return;
    }
    plot_.rect = plot;
    for (ChartSeriesGeometry& entry : geometry_) {
        entry.mark_all();
    }
}

inline void ChartNode::set_time_range(const ChartTimeRange& range) noexcept {
    auto_time_enabled_ = !range.enabled();
    if (auto_time_enabled_) {
        return;
    }
    plot_.time = range;
    for (ChartSeriesGeometry& entry : geometry_) {
        entry.mark_all();
    }
}

inline void ChartNode::set_value_range(const ChartValueRange& range) noexcept {
    auto_value_enabled_ = !range.enabled();
    if (auto_value_enabled_) {
        return;
    }
    plot_.value = range;
    for (ChartSeriesGeometry& entry : geometry_) {
        entry.mark_all();
    }
}

inline void ChartNode::set_auto_ranges() noexcept {
    auto_time_enabled_ = true;
    auto_value_enabled_ = true;
}

inline bool ChartNode::append(std::size_t index, ChartSample sample) noexcept {
    if (index >= series_.size() || !std::isfinite(sample.value)) {
        return false;
    }
    ChartSeriesRing& ring = series_[index];
    const bool was_full = ring.full();
    if (!ring.append(sample)) {
        return false;
    }
    if (was_full) {
        // Eviction renumbers every logical index, so no retained segment keeps both endpoints.
        geometry_[index].mark_all();
    } else {
        geometry_[index].mark_append(ring.size() - 1);
    }
    return true;
}

inline void ChartNode::resolve_ranges() noexcept {
    ChartTimeRange time = plot_.time;
    ChartValueRange value = plot_.value;

    if (auto_time_enabled_) {
        bool any = false;
        double minimum = 0.0;
        double maximum = 0.0;
        for (const ChartSeriesRing& ring : series_) {
            if (ring.empty()) {
                continue;
            }
            const double oldest = ring.oldest().timestamp;
            const double newest = ring.newest().timestamp;
            minimum = any ? std::min(minimum, oldest) : oldest;
            maximum = any ? std::max(maximum, newest) : newest;
            any = true;
        }
        time = any ? ChartTimeRange{minimum, maximum} : ChartTimeRange{};
    }
    if (auto_value_enabled_) {
        bool any = false;
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (const ChartSeriesRing& ring : series_) {
            for (const ChartSample& sample : ring) {
                minimum = any ? std::min(minimum, sample.value) : sample.value;
                maximum = any ? std::max(maximum, sample.value) : sample.value;
                any = true;
            }
        }
        value = any ? ChartValueRange{minimum, maximum} : ChartValueRange{};
    }

    if (time == plot_.time && value == plot_.value) {
        return;
    }
    // A domain change moves every sample on screen, so nothing retained survives it.
    plot_.time = time;
    plot_.value = value;
    for (ChartSeriesGeometry& entry : geometry_) {
        entry.mark_all();
    }
}

inline void ChartNode::rebuild_series(std::size_t index, float thickness,
                                      float marker_size) noexcept {
    ChartSeriesGeometry& entry = geometry_[index];
    const ChartSeriesRing& ring = series_[index];
    const std::size_t points = ring.size();

    if (entry.dirty_all) {
        // Every box must be recomputed from scratch; the stale ones are replaced below, so no
        // old-box damage is recorded (the whole plot is damaged instead, by rebuild()).
        entry.segments.assign(entry.segments.size(), rhi::Rect{});
        entry.markers.assign(entry.markers.size(), rhi::Rect{});
        entry.dirty_first = 0;
        entry.dirty_last = points;
    }
    const std::size_t last = std::min(entry.dirty_last, points);
    for (std::size_t point = entry.dirty_first; point < last; ++point) {
        const ChartSample& sample = ring[point];
        // Segment `point` joins sample `point` to `point + 1`; the final sample has none.
        if (point + 1 < points) {
            const ChartSample& next = ring[point + 1];
            const rhi::Rect box =
                segment_box(plot_.x_for(sample.timestamp), plot_.y_for(sample.value),
                            plot_.x_for(next.timestamp), plot_.y_for(next.value), thickness);
            // Old + new, the same rule ui/layout/damage_tracker applies per node: the union is
            // what a partial redraw must cover. A segment that barely moved therefore damages a
            // band instead of the chart.
            const rhi::Rect previous = entry.segments[point];
            if (!empty_rect(previous)) {
                damage_.add(union_rect(previous, box), plot_.rect);
            }
            entry.segments[point] = box;
            damage_.add(box, plot_.rect);
        }
        if (point < entry.markers.size()) {
            const rhi::Rect box =
                marker_box(plot_.x_for(sample.timestamp), plot_.y_for(sample.value), marker_size);
            const rhi::Rect previous = entry.markers[point];
            if (!empty_rect(previous)) {
                damage_.add(union_rect(previous, box), plot_.rect);
            }
            entry.markers[point] = box;
            damage_.add(box, plot_.rect);
        }
    }
    // Boxes beyond the current sample count are stale. The ring only grows until it is full and
    // then rebuilds whole, so this only clears trailing boxes after a clear().
    for (std::size_t point = last; point < entry.segments.size(); ++point) {
        damage_.add(entry.segments[point], plot_.rect);
        entry.segments[point] = rhi::Rect{};
        damage_.add(entry.markers[point], plot_.rect);
        entry.markers[point] = rhi::Rect{};
    }
    entry.built_points = points;
    entry.clear_dirty();
}

inline const ChartDamage& ChartNode::rebuild() noexcept {
    damage_.clear();
    resolve_ranges();
    // A whole-plot rebuild damages the plot rectangle, because the boxes it replaces are not
    // individually known any more. A chart that has never painted has nothing stale to replace (the
    // owner's first frame paints it whole anyway), so it reports no damage.
    const bool replaces_boxes =
        std::any_of(geometry_.begin(), geometry_.end(),
                    [](const ChartSeriesGeometry& entry) { return entry.built_points > 0; });
    const bool whole =
        std::any_of(geometry_.begin(), geometry_.end(),
                    [](const ChartSeriesGeometry& entry) { return entry.dirty_all; });
    if (whole && replaces_boxes && !empty_rect(plot_.rect)) {
        damage_.add(plot_.rect, plot_.rect);
    }
    for (std::size_t index = 0; index < series_.size(); ++index) {
        if (geometry_[index].dirty()) {
            rebuild_series(index, styles_[index].thickness, styles_[index].marker_size);
        }
    }
    return damage_;
}

inline bool ChartNode::dirty() const noexcept {
    return std::any_of(geometry_.begin(), geometry_.end(),
                       [](const ChartSeriesGeometry& entry) { return entry.dirty(); });
}

inline std::size_t ChartNode::retained_segments() const noexcept {
    std::size_t total = 0;
    for (const ChartSeriesGeometry& entry : geometry_) {
        total += entry.built_points > 1 ? entry.built_points - 1 : 0;
    }
    return total;
}

inline std::size_t ChartNode::retained_markers() const noexcept {
    std::size_t total = 0;
    for (std::size_t index = 0; index < geometry_.size(); ++index) {
        if (styles_[index].marker_size > 0.0f) {
            total += geometry_[index].built_points;
        }
    }
    return total;
}

} // namespace tessera::ui::chart
