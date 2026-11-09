// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tessera::rendering::path {

// Small path model for the vector-rasterizer seam. It deliberately stays below SVG/Canvas
// feature richness: move/line/quadratic/cubic/close, the two fill rules, a solid fill, and a
// basic stroke. Gradients, dashes, text, and masks are not part of the V1 seam and must be added
// deliberately rather than leaked through a third-party type.
struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

// Straight (non-premultiplied) 8-bit color; the alpha channel is an opacity.
struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

enum class FillRule : std::uint8_t {
    non_zero = 0,
    even_odd = 1,
};

enum class LineCap : std::uint8_t {
    butt = 0,
    round = 1,
    square = 2,
};

enum class LineJoin : std::uint8_t {
    miter = 0,
    round = 1,
    bevel = 2,
};

enum class SegmentKind : std::uint8_t {
    move_to = 0,
    line_to = 1,
    quad_to = 2,
    cubic_to = 3,
    close = 4,
};

// One path command. Only the points the command needs are read: move_to/line_to use p0, quad_to
// uses p0 (control) and p1 (end), cubic_to uses p0/p1 (controls) and p2 (end), close uses none.
struct PathSegment {
    SegmentKind kind = SegmentKind::close;
    PointF p0;
    PointF p1;
    PointF p2;
};

struct StrokeStyle {
    Rgba8 color{0, 0, 0, 255};
    float width = 1.0f;
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;
    float miter_limit = 4.0f;
};

// A set of sub-paths sharing one fill rule. A sub-path starts with move_to and ends with close or
// with the end of the segment list; commands before the first move_to are invalid.
struct Path {
    std::vector<PathSegment> segments;
    FillRule fill_rule = FillRule::non_zero;
};

struct PaintStyle {
    std::optional<Rgba8> fill;
    std::optional<StrokeStyle> stroke;
};

// One rasterization input: geometry plus the paint applied to it.
struct PathShape {
    Path path;
    PaintStyle paint;
};

// Caller-owned RGBA8 target, 4 bytes per pixel, straight alpha. `stride_bytes` may be larger than
// `width * 4` (for example when rasterizing into a sub-rectangle of a bigger image) and must be a
// multiple of 4. The pixels must be 4-byte aligned. The caller initializes the pixels; the
// rasterizer only composites over them.
struct RasterTarget {
    std::span<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;

    [[nodiscard]] bool valid() const noexcept {
        const auto row_bytes = static_cast<std::size_t>(width) * 4U;
        const bool aligned = (reinterpret_cast<std::uintptr_t>(pixels.data()) % 4U) == 0U;
        return width > 0U && height > 0U && (stride_bytes % 4U) == 0U &&
               stride_bytes >= row_bytes && aligned &&
               pixels.size() >= static_cast<std::size_t>(stride_bytes) * height;
    }
};

} // namespace tessera::rendering::path
