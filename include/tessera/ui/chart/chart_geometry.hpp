// SPDX-License-Identifier: MIT
//
// Retained plot geometry for a chart node (issue #183). Normative contract:
// docs/specs/chart-node-v1.md.
//
// The chart keeps one device-space box per polyline segment per series and rebuilds only the
// segments an append can have moved. That retained set is what lets a chart repaint a band instead
// of its whole box: the boxes are already where the pixels are, so the damage rule is "the union of
// the old box and the new box of every rebuilt segment" - the same old+new rule
// ui/layout/damage_pass.hpp applies per node, applied per segment instead.
//
// Everything here is allocation-free after construction: append() records a dirty index range,
// rebuild() writes into the pre-sized vectors, and ChartDamage::add() merges into a fixed array.
// Nothing consults a clock, so a rebuild is a pure function of the samples and the plot rectangle.

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/chart/chart_series.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace tessera::ui::chart {

namespace rhi = tessera::rendering::rhi;

// A closed numeric span. `enabled()` is the "explicitly configured" test used everywhere here: an
// empty range means auto-fit, which is the only mode that can react to new samples without the
// caller re-specifying the window it wants.
template<typename T> struct ChartRange {
    T min = T{};
    T max = T{};

    [[nodiscard]] bool enabled() const noexcept {
        return max > min;
    }
    [[nodiscard]] friend bool operator==(const ChartRange&, const ChartRange&) = default;
};

using ChartTimeRange = ChartRange<double>;
using ChartValueRange = ChartRange<float>;

[[nodiscard]] inline bool empty_rect(const rhi::Rect& rect) noexcept {
    return !(rect.width > 0.0f) || !(rect.height > 0.0f);
}

[[nodiscard]] inline rhi::Rect union_rect(const rhi::Rect& lhs, const rhi::Rect& rhs) noexcept {
    const float x0 = std::min(lhs.x, rhs.x);
    const float y0 = std::min(lhs.y, rhs.y);
    const float x1 = std::max(lhs.right(), rhs.right());
    const float y1 = std::max(lhs.bottom(), rhs.bottom());
    return rhi::Rect{x0, y0, x1 - x0, y1 - y0};
}

[[nodiscard]] inline bool rects_touch(const rhi::Rect& lhs, const rhi::Rect& rhs) noexcept {
    return lhs.x <= rhs.right() && rhs.x <= lhs.right() && lhs.y <= rhs.bottom() &&
           rhs.y <= lhs.bottom();
}

[[nodiscard]] inline bool rects_intersect(const rhi::Rect& lhs, const rhi::Rect& rhs) noexcept {
    return lhs.x < rhs.right() && rhs.x < lhs.right() && lhs.y < rhs.bottom() &&
           rhs.y < lhs.bottom();
}

// Linear map from the two data domains to the plot rectangle. The y axis is inverted because a
// larger value is higher on screen. A degenerate domain maps to the plot's leading edge rather than
// dividing by zero, so a single-sample or flat series still produces a determinate box.
struct ChartPlot {
    rhi::Rect rect;
    ChartTimeRange time;
    ChartValueRange value;

    [[nodiscard]] float x_for(double timestamp) const noexcept {
        const double span = time.max - time.min;
        if (!(span > 0.0)) {
            return rect.x;
        }
        const double t = (timestamp - time.min) / span;
        return rect.x + static_cast<float>(std::clamp(t, 0.0, 1.0)) * rect.width;
    }

    [[nodiscard]] float y_for(float data) const noexcept {
        const double span = static_cast<double>(value.max) - static_cast<double>(value.min);
        if (!(span > 0.0)) {
            return rect.bottom();
        }
        const double t = (static_cast<double>(data) - static_cast<double>(value.min)) / span;
        // 0 = the value minimum, 1 = the maximum; the maximum is the plot's top edge.
        return rect.y + (1.0f - static_cast<float>(std::clamp(t, 0.0, 1.0))) * rect.height;
    }

    [[nodiscard]] bool operator==(const ChartPlot&) const = default;
};

// One series' retained segment boxes and the range that still needs rebuilding.
//
// `segments[i]` is the box of the polyline segment between sample i and sample i + 1, inflated by
// half the stroke thickness; `markers[i]` is sample i's marker box (empty when markers are off).
// Both vectors are sized to the series capacity at construction, so a rebuild never grows them.
struct ChartSeriesGeometry {
    std::vector<rhi::Rect> segments;
    std::vector<rhi::Rect> markers;
    // Samples the boxes were built from, and the first/last segment index still to rebuild.
    // `dirty_last <= dirty_first` means "nothing to do".
    std::size_t built_points = 0;
    std::size_t dirty_first = 0;
    std::size_t dirty_last = 0;
    // A rebuild of this series must recompute every segment (the time window or the plot rectangle
    // moved, the style changed, or the ring evicted its oldest sample).
    bool dirty_all = true;

    void reserve(std::size_t capacity) {
        segments.resize(capacity);
        markers.resize(capacity);
        built_points = 0;
        dirty_all = true;
    }

    [[nodiscard]] bool dirty() const noexcept {
        return dirty_all || dirty_last > dirty_first;
    }
    void mark_all() noexcept {
        dirty_all = true;
    }
    // Marks the segments an append of logical index `appended` can disturb. The new sample ends
    // exactly one segment; when the append also evicted the oldest sample, every index shifted and
    // the caller marks the series whole instead.
    void mark_append(std::size_t appended) noexcept {
        if (dirty_all) {
            return;
        }
        const std::size_t first = appended == 0 ? 0 : appended - 1;
        const std::size_t last = appended;
        if (dirty_last > dirty_first) {
            dirty_first = std::min(dirty_first, first);
            dirty_last = std::max(dirty_last, last);
        } else {
            dirty_first = first;
            dirty_last = last;
        }
    }
    void clear_dirty() noexcept {
        dirty_all = false;
        dirty_first = 0;
        dirty_last = 0;
    }
};

// A fixed, deterministic damage set. Rectangles are AABB-merged when they touch, exactly like
// rendering::DamageAccumulator, but without its target clipping: the chart's boxes are already
// inside the plot rectangle apart from the stroke inflation (half the thickness a stroke centred on
// the plot edge reaches past it, which is why an owner that wants a hard edge clips the chart).
// When the cap is exceeded the whole `fallback` rectangle is used, which is over-damage only, never
// under-damage.
class ChartDamage {
public:
    static constexpr std::size_t kMaxRects = 8;

    void clear() noexcept {
        count_ = 0;
        full_ = false;
    }

    // `fallback` is the rectangle used when the set overflows; callers pass the plot rectangle.
    void add(const rhi::Rect& rect, const rhi::Rect& fallback) noexcept {
        if (full_ || empty_rect(rect)) {
            return;
        }
        rhi::Rect merged = rect;
        bool joined = true;
        while (joined) {
            joined = false;
            for (std::size_t index = 0; index < count_; ++index) {
                if (!rects_touch(merged, rects_[index])) {
                    continue;
                }
                merged = union_rect(merged, rects_[index]);
                rects_[index] = rects_[--count_];
                joined = true;
                break;
            }
        }
        if (count_ == kMaxRects) {
            full_ = true;
            count_ = 1;
            rects_[0] = fallback;
            return;
        }
        rects_[count_++] = merged;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] bool full() const noexcept {
        return full_;
    }
    [[nodiscard]] const rhi::Rect* begin() const noexcept {
        return rects_.data();
    }
    [[nodiscard]] const rhi::Rect* end() const noexcept {
        return rects_.data() + count_;
    }
    [[nodiscard]] const rhi::Rect& operator[](std::size_t index) const noexcept {
        return rects_[index];
    }

private:
    std::array<rhi::Rect, kMaxRects> rects_{};
    std::size_t count_ = 0;
    bool full_ = false;
};

// The box a polyline segment occupies once stroked with `thickness`: the segment's axis-aligned
// bounding box inflated by half the stroke on every side. Horizontal and vertical segments are then
// exact; a diagonal segment covers its whole bounding box (the V1 approximation of
// docs/specs/chart-node-v1.md section 4). Non-finite input collapses to an empty box.
[[nodiscard]] inline rhi::Rect segment_box(float x0, float y0, float x1, float y1,
                                           float thickness) noexcept {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) {
        return rhi::Rect{};
    }
    const float half = std::max(thickness, 0.0f) * 0.5f;
    const float left = std::min(x0, x1) - half;
    const float top = std::min(y0, y1) - half;
    const float right = std::max(x0, x1) + half;
    const float bottom = std::max(y0, y1) + half;
    return rhi::Rect{left, top, right - left, bottom - top};
}

// The square painted for one sample marker, centred on the mapped point.
[[nodiscard]] inline rhi::Rect marker_box(float x, float y, float size) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !(size > 0.0f)) {
        return rhi::Rect{};
    }
    const float half = size * 0.5f;
    return rhi::Rect{x - half, y - half, size, size};
}

} // namespace tessera::ui::chart
