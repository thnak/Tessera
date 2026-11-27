// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace tessera::rendering::gpu::clip {

// The shaped-clip coverage rule (ADR-068 slice 4, issue #672).
//
// One function decides whether a pixel is inside a clip record, and the fragment shader and
// `CpuCanvas` both implement it. Keeping the rule in one place is what makes a golden meaningful: a
// CPU reference that disagreed with the shader by a rounding step would make every shaped-clip
// golden a tolerance argument instead of a test.
//
// The rule is the pixel-centre test: a pixel at integer coordinate (px, py) is covered when its
// centre (px + 0.5, py + 0.5) is inside the shape. That is the same rule `fill_rect` uses, so a
// shaped clip and a filled rect agree on which pixels a shared edge belongs to.
//
// Coverage is binary here (0 or 1). The anti-aliased path is the shader's `clip_coverage`, which
// blends the signed distance over +/- aa_radius; this function is the hard test the golden path
// (`aa_radius == 0`) uses, and the CPU reference has no AA at all.

// A circle or ellipse, in target pixels. `cx`/`cy` are the centre, `rx`/`ry` the radii.
struct Ellipse {
    float cx = 0.0f;
    float cy = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;

    [[nodiscard]] bool has_area() const {
        return rx > 0.0f && ry > 0.0f;
    }
};

// True when the point is inside the ellipse. The test is the normalised squared distance, which is
// exact for an axis-aligned ellipse and needs no square root.
[[nodiscard]] inline bool inside_ellipse(const Ellipse& ellipse, float x, float y) {
    if (!ellipse.has_area()) {
        return false;
    }
    const float dx = (x - ellipse.cx) / ellipse.rx;
    const float dy = (y - ellipse.cy) / ellipse.ry;
    return dx * dx + dy * dy <= 1.0f;
}

// The ellipse a `circle()`/`ellipse()` clip describes: a circle is the special case rx == ry.
[[nodiscard]] inline Ellipse make_ellipse(const rhi::Rect& rect) {
    return Ellipse{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f, rect.width * 0.5f,
                   rect.height * 0.5f};
}

// True when the point is inside a rounded rectangle with per-corner *elliptical* radii.
//
// `radii[0..3]` are the horizontal radii (TL, TR, BR, BL) and `radii[4..7]` the vertical ones; a
// circular corner has the two equal. A zero pair is a square corner. The corner test is the
// normalised squared distance against that corner's ellipse, so an elliptical corner is exact.
//
// This is the same rule the shader's `inside_clip` implements, extended from circular to elliptical
// corners. A record whose vertical radii are all zero is treated as circular (the slice 3 layout),
// so an old record keeps its meaning.
[[nodiscard]] inline bool inside_rounded_rect(const ClipRecord& record, float x, float y) {
    const float x0 = record.x;
    const float y0 = record.y;
    const float x1 = record.x + record.width;
    const float y1 = record.y + record.height;
    if (x < x0 || y < y0 || x > x1 || y > y1) {
        return false;
    }
    // The vertical radius falls back to the horizontal one when the record carries no elliptical
    // pair, which is what a slice 3 record looks like.
    const auto vertical = [&record](int corner) {
        const float value = record.radii[4 + corner];
        return value > 0.0f ? value : record.radii[corner];
    };
    const float qx = x - x0;
    const float qy = y - y0;
    const float width = record.width;
    const float height = record.height;

    // Top-left.
    const float tl_x = record.radii[0];
    const float tl_y = vertical(0);
    if (tl_x > 0.0f && qx < tl_x && qy < tl_y) {
        const float dx = (qx - tl_x) / tl_x;
        const float dy = (qy - tl_y) / tl_y;
        return dx * dx + dy * dy <= 1.0f;
    }
    // Top-right.
    const float tr_x = record.radii[1];
    const float tr_y = vertical(1);
    if (tr_x > 0.0f && qx > width - tr_x && qy < tr_y) {
        const float dx = (qx - (width - tr_x)) / tr_x;
        const float dy = (qy - tr_y) / tr_y;
        return dx * dx + dy * dy <= 1.0f;
    }
    // Bottom-right.
    const float br_x = record.radii[2];
    const float br_y = vertical(2);
    if (br_x > 0.0f && qx > width - br_x && qy > height - br_y) {
        const float dx = (qx - (width - br_x)) / br_x;
        const float dy = (qy - (height - br_y)) / br_y;
        return dx * dx + dy * dy <= 1.0f;
    }
    // Bottom-left.
    const float bl_x = record.radii[3];
    const float bl_y = vertical(3);
    if (bl_x > 0.0f && qx < bl_x && qy > height - bl_y) {
        const float dx = (qx - bl_x) / bl_x;
        const float dy = (qy - (height - bl_y)) / bl_y;
        return dx * dx + dy * dy <= 1.0f;
    }
    return true;
}

// The signed distance from `(px, py)` (relative to the box centre) to a rounded box of half size
// `(half_width, half_height)` and corner radius `radius`: negative inside, positive outside. This
// is the fragment shader's `sd_round_box`, expression for expression.
[[nodiscard]] inline float signed_distance_round_box(float px, float py, float half_width,
                                                     float half_height, float radius) {
    const float qx = std::abs(px) - half_width + radius;
    const float qy = std::abs(py) - half_height + radius;
    const float outside_x = std::max(qx, 0.0f);
    const float outside_y = std::max(qy, 0.0f);
    return std::min(std::max(qx, qy), 0.0f) +
           std::sqrt(outside_x * outside_x + outside_y * outside_y) - radius;
}

// GLSL `smoothstep(edge0, edge1, x)`.
[[nodiscard]] inline float smooth_step(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The coverage of a solid rounded box at a point relative to its centre, as the shader's rounded
// quad (vertex mode 1) computes it: the hard pixel-centre test (`d <= 0`) when `aa_radius` is 0,
// otherwise `1 - smoothstep(-aa_radius, aa_radius, d)` over the signed distance. `aa_radius` is the
// half-width of the blend in pixels (the renderer uses 0.5 with anti-aliasing on, 0 in golden
// mode).
[[nodiscard]] inline float rounded_box_coverage(float px, float py, float half_width,
                                                float half_height, float radius, float aa_radius) {
    const float distance = signed_distance_round_box(px, py, half_width, half_height, radius);
    if (aa_radius <= 0.0f) {
        return distance <= 0.0f ? 1.0f : 0.0f;
    }
    return 1.0f - smooth_step(-aa_radius, aa_radius, distance);
}

// The coverage one `RoundedRect` clip record gives the point (x, y) in target pixels: the shader's
// `inside_clip` hard test (exact per-corner elliptical radii, `inside_rounded_rect`) when
// `aa_radius` is 0, and its `clip_coverage` otherwise - the rounded-box distance with the radius of
// the pixel's quadrant, blended over +/- `aa_radius` pixels. A pixel-aligned clip edge is fully
// covered on its inside pixels either way.
[[nodiscard]] inline float rounded_rect_coverage(const ClipRecord& record, float x, float y,
                                                 float aa_radius) {
    if (aa_radius <= 0.0f) {
        return inside_rounded_rect(record, x, y) ? 1.0f : 0.0f;
    }
    const float half_width = record.width * 0.5f;
    const float half_height = record.height * 0.5f;
    const float qx = x - (record.x + half_width);
    const float qy = y - (record.y + half_height);
    const float radius_for_quadrant = qx < 0.0f ? (qy < 0.0f ? record.radii[0] : record.radii[3])
                                                : (qy < 0.0f ? record.radii[1] : record.radii[2]);
    const float radius = std::min(radius_for_quadrant, std::min(half_width, half_height));
    return rounded_box_coverage(qx, qy, half_width, half_height, radius, aa_radius);
}

// True when the point is inside the polygon `record` names in `pool` (ADR-068 slice 5).
//
// A ray is cast toward +x from the point and each edge that straddles the point's y (half-open: an
// edge counts when exactly one endpoint is at or above it, so a vertex on the ray is counted once)
// and lies to the point's right crosses it. `evenodd` is the parity of the crossings; `nonzero` is
// the signed sum of their directions (the winding number). The shader's `inside_polygon_record`
// is this loop, expression for expression, so the two agree on every pixel centre that is not
// within rounding of an edge.
[[nodiscard]] inline bool inside_polygon(const ClipRecord& record, std::span<const ClipVertex> pool,
                                         float x, float y) {
    const std::uint32_t count = record.vertex_count;
    bool parity = false;
    int winding = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        const ClipVertex a = pool[record.first_vertex + index];
        const ClipVertex b = pool[record.first_vertex + (index + 1 == count ? 0 : index + 1)];
        if ((a.y <= y) == (b.y <= y)) {
            continue;
        }
        const float t = (y - a.y) / (b.y - a.y);
        const float crossing_x = a.x + t * (b.x - a.x);
        if (x < crossing_x) {
            parity = !parity;
            winding += b.y > a.y ? 1 : -1;
        }
    }
    return (record.flags & kRecordFlagEvenOdd) != 0 ? parity : winding != 0;
}

// True when `record` names a polygon whose vertices are all inside `pool`. A polygon that does not
// (the table's over-limit fallback, or a record with no pool) is treated as its bounding box.
[[nodiscard]] inline bool has_polygon_vertices(const ClipRecord& record,
                                               std::span<const ClipVertex> pool) {
    return record.vertex_count >= 3 && record.first_vertex <= pool.size() &&
           record.vertex_count <= pool.size() - record.first_vertex;
}

// True when the pixel at integer coordinate (px, py) is covered by `record`, by the pixel-centre
// rule. This is the one function the CPU reference and the hard-test shader path agree on.
// `pool` is the frame's vertex pool; only a `Polygon` record reads it.
[[nodiscard]] inline bool covers_pixel(const ClipRecord& record, std::int32_t px, std::int32_t py,
                                       std::span<const ClipVertex> pool = {}) {
    const float x = static_cast<float>(px) + 0.5f;
    const float y = static_cast<float>(py) + 0.5f;
    switch (static_cast<RecordKind>(record.kind)) {
    case RecordKind::Ellipse: {
        const Ellipse ellipse =
            make_ellipse(rhi::Rect{record.x, record.y, record.width, record.height});
        return inside_ellipse(ellipse, x, y);
    }
    case RecordKind::Polygon:
        if (has_polygon_vertices(record, pool)) {
            return inside_polygon(record, pool, x, y);
        }
        // The over-limit fallback: the bounding box, the conservative rule the ADR documents.
        return inside_rounded_rect(record, x, y);
    case RecordKind::RoundedRect:
        return inside_rounded_rect(record, x, y);
    }
    return true;
}

// The smallest integer scissor containing the record's bounding box. A shaped clip still narrows
// the hardware scissor, so the shader only refines the edge.
[[nodiscard]] inline rhi::Scissor enclosing_scissor(const ClipRecord& record) {
    if (record.width <= 0.0f || record.height <= 0.0f) {
        return rhi::Scissor{};
    }
    const auto x0 = static_cast<std::int32_t>(std::floor(record.x));
    const auto y0 = static_cast<std::int32_t>(std::floor(record.y));
    const auto x1 = static_cast<std::int32_t>(std::ceil(record.x + record.width));
    const auto y1 = static_cast<std::int32_t>(std::ceil(record.y + record.height));
    if (x1 <= x0 || y1 <= y0) {
        return rhi::Scissor{};
    }
    return rhi::Scissor{x0, y0, static_cast<std::uint32_t>(x1 - x0),
                        static_cast<std::uint32_t>(y1 - y0)};
}

} // namespace tessera::rendering::gpu::clip
