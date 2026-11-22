// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cmath>
#include <cstdint>

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

// True when the pixel at integer coordinate (px, py) is covered by `record`, by the pixel-centre
// rule. This is the one function the CPU reference and the hard-test shader path agree on.
[[nodiscard]] inline bool covers_pixel(const ClipRecord& record, std::int32_t px, std::int32_t py) {
    const float x = static_cast<float>(px) + 0.5f;
    const float y = static_cast<float>(py) + 0.5f;
    switch (static_cast<RecordKind>(record.kind)) {
    case RecordKind::Ellipse: {
        const Ellipse ellipse = make_ellipse(rhi::Rect{record.x, record.y, record.width,
                                                       record.height});
        return inside_ellipse(ellipse, x, y);
    }
    case RecordKind::RoundedRect:
    case RecordKind::Polygon:
        // Polygon records are slice 5; until then a polygon record is treated as its bounding box,
        // which is the conservative rule the ADR documents for an unsupported shape.
        return inside_rounded_rect(record, x, y);
    }
    return true;
}

// The smallest integer scissor containing the record's bounding box. A shaped clip still narrows the
// hardware scissor, so the shader only refines the edge.
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
