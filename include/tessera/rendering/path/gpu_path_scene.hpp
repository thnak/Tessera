// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace tessera::rendering::path {

// The GPU path rasterizer's input (ADR-071): shapes flattened to line segments in the shape's own
// space, with the paint and stroke state the compute passes read. Pure data; no GPU object is
// involved, so the encoder is unit-testable without a device.

// x' = a x + c y + e, y' = b x + d y + f: one per shape, applied by the GPU, so animating a shape's
// transform rewrites six words and re-encodes nothing.
struct PathAffine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

    [[nodiscard]] bool is_identity() const noexcept {
        return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f;
    }
    [[nodiscard]] PointF apply(PointF p) const noexcept {
        return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }
};

enum class GpuFillRule : std::uint8_t { non_zero = 0, even_odd = 1 };
enum class GpuJoin : std::uint8_t { round = 0, bevel = 1, miter = 2 };
enum class GpuCap : std::uint8_t { round = 0, butt = 1, square = 2 };

inline constexpr std::size_t kGpuMaxDashEntries = 8;

struct GpuPathShape {
    // x0 y0 x1 y1 per segment, closed contours back to back.
    std::vector<float> contours;
    std::uint32_t rgba = 0; // straight alpha, R in the low byte (the order of the RGBA8 output)
    GpuFillRule rule = GpuFillRule::non_zero;
    // > 0: `contours` is a stroke centerline (one segment per edge, in order) with this half width,
    // expanded on the GPU; `open` puts a cap at the end of an open contour.
    float half_width = 0.0f;
    bool open = false;
    // Several contours in one shape (an SVG path with subpaths): segments per contour, and whether
    // each is open. Empty means one contour of every segment, open when `open` says so.
    std::vector<std::uint32_t> contour_sizes;
    std::vector<bool> contour_open;
    // Normalized on/off lengths: an even count of at most kGpuMaxDashEntries, empty = solid.
    std::vector<float> dash;
    float dash_offset = 0.0f;
    GpuJoin join = GpuJoin::round;
    GpuCap cap = GpuCap::round;
    float miter_limit = 4.0f;
};

struct GpuPathScene {
    std::vector<GpuPathShape> shapes;
};

// The straight-alpha RGBA8 word of `color` the shaders composite with.
[[nodiscard]] std::uint32_t pack_rgba(Rgba8 color) noexcept;

// Odd dash arrays repeat to make an even one (the SVG rule); more than kGpuMaxDashEntries entries
// are truncated to the first kGpuMaxDashEntries (even count kept), and an all-zero pattern is
// solid.
[[nodiscard]] std::vector<float> normalized_dash_pattern(std::vector<float> pattern);

// Flattens decoded document shapes (already scaled to device pixels) into one scene: per source
// shape a fill shape and then a stroke shape, in painter order. Curves are split to within
// `tolerance` pixels of chord error; a repeated point is dropped, since a zero-length segment has
// no direction and could not carry a join. A shape with neither fill nor stroke adds nothing.
[[nodiscard]] GpuPathScene gpu_scene_from_shapes(std::span<const PathShape> shapes,
                                                 float tolerance = 0.05f);

} // namespace tessera::rendering::path
