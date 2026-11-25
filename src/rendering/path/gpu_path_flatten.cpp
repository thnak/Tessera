// SPDX-License-Identifier: MIT
//
// Decoded path shapes -> the GPU path rasterizer's scene (ADR-071): flatten curves to line segments
// in device pixels and split each source shape into a fill shape and a stroke shape.

#include "tessera/rendering/path/gpu_path_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace tessera::rendering::path {
namespace {

constexpr float kSamePoint = 1e-4f; // closer than this is the same point

struct FlatContour {
    std::vector<PointF> points;
    bool closed = false;
};

[[nodiscard]] float distance(PointF a, PointF b) noexcept {
    return std::hypot(a.x - b.x, a.y - b.y);
}

[[nodiscard]] float second_difference(PointF a, PointF b, PointF c) noexcept {
    return std::hypot(a.x - 2 * b.x + c.x, a.y - 2 * b.y + c.y);
}

// Curves split by Wang's bound for a chord error of `tolerance` pixels.
[[nodiscard]] std::vector<FlatContour> flatten_path(const Path& geometry, float tolerance) {
    std::vector<FlatContour> result;
    FlatContour current;
    PointF at{0, 0}, start{0, 0};
    const auto push = [&](PointF p) {
        if (current.points.empty() || distance(p, current.points.back()) > kSamePoint) {
            current.points.push_back(p);
        }
        at = p;
    };
    const auto begin_drawing = [&] {
        if (current.points.empty()) {
            current.points.push_back(at); // after a close, drawing resumes from the subpath start
        }
    };
    const auto finish = [&] {
        if (!current.points.empty()) {
            result.push_back(std::move(current));
        }
        current = {};
    };
    for (const PathSegment& segment : geometry.segments) {
        switch (segment.kind) {
        case SegmentKind::move_to:
            finish();
            at = start = segment.p0;
            current.points.push_back(at);
            break;
        case SegmentKind::line_to:
            begin_drawing();
            push(segment.p0);
            break;
        case SegmentKind::quad_to: {
            begin_drawing();
            const PointF p0 = at, c = segment.p0, p1 = segment.p1;
            const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(
                                         0.25f * second_difference(p0, c, p1) / tolerance))),
                                     1, 64);
            for (int i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
                push({u * u * p0.x + 2 * u * t * c.x + t * t * p1.x,
                      u * u * p0.y + 2 * u * t * c.y + t * t * p1.y});
            }
            break;
        }
        case SegmentKind::cubic_to: {
            begin_drawing();
            const PointF p0 = at, c0 = segment.p0, c1 = segment.p1, p1 = segment.p2;
            const float m = std::max(second_difference(p0, c0, c1), second_difference(c0, c1, p1));
            const int n =
                std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75f * m / tolerance))), 1, 128);
            for (int i = 1; i <= n; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
                const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
                push({b0 * p0.x + b1 * c0.x + b2 * c1.x + b3 * p1.x,
                      b0 * p0.y + b1 * c0.y + b2 * c1.y + b3 * p1.y});
            }
            break;
        }
        case SegmentKind::close:
            if (!current.points.empty()) {
                current.closed = true;
                finish();
            }
            at = start;
            break;
        }
    }
    finish();
    return result;
}

// Appends the segments of `points` (closed back to the first point when `close`) to `out` and
// returns how many were added.
std::size_t add_segments(std::vector<float>& out, std::vector<PointF> points, bool close) {
    if (close && points.size() > 1 && distance(points.back(), points[0]) <= kSamePoint) {
        points.pop_back(); // the closing segment would have no length
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        out.insert(out.end(), {points[i].x, points[i].y, points[i + 1].x, points[i + 1].y});
        ++count;
    }
    if (close && points.size() > 1) {
        out.insert(out.end(), {points.back().x, points.back().y, points[0].x, points[0].y});
        ++count;
    }
    return count;
}

[[nodiscard]] GpuJoin join_of(LineJoin join) noexcept {
    return join == LineJoin::round   ? GpuJoin::round
           : join == LineJoin::bevel ? GpuJoin::bevel
                                     : GpuJoin::miter;
}

[[nodiscard]] GpuCap cap_of(LineCap cap) noexcept {
    return cap == LineCap::round  ? GpuCap::round
           : cap == LineCap::butt ? GpuCap::butt
                                  : GpuCap::square;
}

} // namespace

std::uint32_t pack_rgba(Rgba8 c) noexcept {
    return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
           (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
}

std::vector<float> normalized_dash_pattern(std::vector<float> pattern) {
    if (pattern.size() % 2 == 1) {
        const std::size_t count = pattern.size();
        for (std::size_t i = 0; i < count; ++i) {
            pattern.push_back(pattern[i]);
        }
    }
    if (pattern.size() > kGpuMaxDashEntries) {
        pattern.resize(kGpuMaxDashEntries);
    }
    float total = 0.0f;
    for (const float length : pattern) {
        if (!(length >= 0.0f)) {
            return {}; // a negative or NaN length makes the pattern invalid: draw solid
        }
        total += length;
    }
    if (!(total > 0.0f)) {
        return {};
    }
    return pattern;
}

GpuPathScene gpu_scene_from_shapes(std::span<const PathShape> shapes, float tolerance) {
    GpuPathScene scene;
    const float flat_tolerance = tolerance > 0.0f ? tolerance : 0.05f;
    for (const PathShape& source : shapes) {
        if (!source.paint.fill && !source.paint.stroke) {
            continue;
        }
        const std::vector<FlatContour> contours = flatten_path(source.path, flat_tolerance);
        if (source.paint.fill) {
            GpuPathShape fill;
            fill.rgba = pack_rgba(*source.paint.fill);
            fill.rule = source.path.fill_rule == FillRule::even_odd ? GpuFillRule::even_odd
                                                                    : GpuFillRule::non_zero;
            for (const FlatContour& contour : contours) {
                add_segments(fill.contours, contour.points, true); // a fill closes every subpath
            }
            if (!fill.contours.empty()) {
                scene.shapes.push_back(std::move(fill));
            }
        }
        if (source.paint.stroke && source.paint.stroke->width > 0.0f) {
            const StrokeStyle& style = *source.paint.stroke;
            GpuPathShape stroke;
            stroke.rgba = pack_rgba(style.color);
            stroke.half_width = style.width * 0.5f;
            stroke.join = join_of(style.join);
            stroke.cap = cap_of(style.cap);
            stroke.miter_limit = style.miter_limit;
            for (const FlatContour& contour : contours) {
                const std::size_t count =
                    add_segments(stroke.contours, contour.points, contour.closed);
                if (count > 0) {
                    stroke.contour_sizes.push_back(static_cast<std::uint32_t>(count));
                    stroke.contour_open.push_back(!contour.closed);
                }
            }
            if (!stroke.contours.empty()) {
                scene.shapes.push_back(std::move(stroke));
            }
        }
    }
    return scene;
}

} // namespace tessera::rendering::path
