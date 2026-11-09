// SPDX-License-Identifier: MIT
//
// Representative icon/chart path set for the ThorVG spike. The set is deliberately small but
// covers every feature the PathRasterizer seam exposes: move/line/quad/cubic/close, non-zero and
// even-odd fill, miter/round joins, butt/round/square caps, stroked-only paths, curved fills, and
// straight-alpha compositing. Nothing here is generated at runtime.

#include "scene.hpp"

#include <utility>
#include <vector>

namespace tessera::poc::thorvg_spike {
namespace {

using tessera::rendering::path::FillRule;
using tessera::rendering::path::LineCap;
using tessera::rendering::path::LineJoin;
using tessera::rendering::path::PaintStyle;
using tessera::rendering::path::Path;
using tessera::rendering::path::PathSegment;
using tessera::rendering::path::PathShape;
using tessera::rendering::path::PointF;
using tessera::rendering::path::Rgba8;
using tessera::rendering::path::SegmentKind;
using tessera::rendering::path::StrokeStyle;

// Cubic circle approximation constant (a quarter circle as one cubic).
constexpr float kArc = 0.5522847498f;

[[nodiscard]] PathSegment move_to(float x, float y) {
    return {SegmentKind::move_to, {x, y}, {}, {}};
}

[[nodiscard]] PathSegment line_to(float x, float y) {
    return {SegmentKind::line_to, {x, y}, {}, {}};
}

[[nodiscard]] PathSegment quad_to(float cx, float cy, float x, float y) {
    return {SegmentKind::quad_to, {cx, cy}, {x, y}, {}};
}

[[nodiscard]] PathSegment cubic_to(float cx1, float cy1, float cx2, float cy2, float x, float y) {
    return {SegmentKind::cubic_to, {cx1, cy1}, {cx2, cy2}, {x, y}};
}

[[nodiscard]] PathSegment close_path() {
    return {SegmentKind::close, {}, {}, {}};
}

void append_rect(Path& path, float x, float y, float w, float h) {
    path.segments.push_back(move_to(x, y));
    path.segments.push_back(line_to(x + w, y));
    path.segments.push_back(line_to(x + w, y + h));
    path.segments.push_back(line_to(x, y + h));
    path.segments.push_back(close_path());
}

void append_rounded_rect(Path& path, float x, float y, float w, float h, float radius) {
    const float k = kArc * radius;
    path.segments.push_back(move_to(x + radius, y));
    path.segments.push_back(line_to(x + w - radius, y));
    path.segments.push_back(
        cubic_to(x + w - radius + k, y, x + w, y + radius - k, x + w, y + radius));
    path.segments.push_back(line_to(x + w, y + h - radius));
    path.segments.push_back(
        cubic_to(x + w, y + h - radius + k, x + w - radius + k, y + h, x + w - radius, y + h));
    path.segments.push_back(line_to(x + radius, y + h));
    path.segments.push_back(
        cubic_to(x + radius - k, y + h, x, y + h - radius + k, x, y + h - radius));
    path.segments.push_back(line_to(x, y + radius));
    path.segments.push_back(cubic_to(x, y + radius - k, x + radius - k, y, x + radius, y));
    path.segments.push_back(close_path());
}

void append_circle(Path& path, float cx, float cy, float radius) {
    const float k = kArc * radius;
    path.segments.push_back(move_to(cx + radius, cy));
    path.segments.push_back(cubic_to(cx + radius, cy + k, cx + k, cy + radius, cx, cy + radius));
    path.segments.push_back(cubic_to(cx - k, cy + radius, cx - radius, cy + k, cx - radius, cy));
    path.segments.push_back(cubic_to(cx - radius, cy - k, cx - k, cy - radius, cx, cy - radius));
    path.segments.push_back(cubic_to(cx + k, cy - radius, cx + radius, cy - k, cx + radius, cy));
    path.segments.push_back(close_path());
}

// Self-intersecting 5-point star (pentagram): the outer vertices are visited in skip order, so the
// even-odd fill rule leaves the center pentagon hollow while non-zero would fill it solid.
void append_pentagram(Path& path, float cx, float cy, float radius) {
    constexpr float kOuterX = 0.95106f; // sin(72 degrees)
    constexpr float kOuterY = 0.30902f; // cos(72 degrees)
    constexpr float kMidX = 0.58779f;   // sin(36 degrees)
    constexpr float kMidY = 0.80902f;   // cos(36 degrees)
    path.segments.push_back(move_to(cx, cy - radius));
    path.segments.push_back(line_to(cx + kMidX * radius, cy + kMidY * radius));
    path.segments.push_back(line_to(cx - kOuterX * radius, cy - kOuterY * radius));
    path.segments.push_back(line_to(cx + kOuterX * radius, cy - kOuterY * radius));
    path.segments.push_back(line_to(cx - kMidX * radius, cy + kMidY * radius));
    path.segments.push_back(close_path());
}

void scale_path(Path& path, float sx, float sy) {
    for (PathSegment& segment : path.segments) {
        segment.p0 = {segment.p0.x * sx, segment.p0.y * sy};
        segment.p1 = {segment.p1.x * sx, segment.p1.y * sy};
        segment.p2 = {segment.p2.x * sx, segment.p2.y * sy};
    }
}

[[nodiscard]] Rgba8 rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return {r, g, b, a};
}

[[nodiscard]] StrokeStyle stroke(Rgba8 color, float width, LineCap cap = LineCap::butt,
                                 LineJoin join = LineJoin::miter) {
    return StrokeStyle{color, width, cap, join, 4.0f};
}

[[nodiscard]] PathShape shape(Path path, PaintStyle paint) {
    return PathShape{std::move(path), paint};
}

// App icon tile: rounded-rect fill with a miter-joined stroke.
[[nodiscard]] PathShape app_tile() {
    Path path;
    append_rounded_rect(path, 24.0f, 24.0f, 216.0f, 216.0f, 36.0f);
    PaintStyle paint;
    paint.fill = rgba(30, 41, 59);
    paint.stroke = stroke(rgba(56, 189, 248), 3.0f, LineCap::butt, LineJoin::miter);
    return shape(std::move(path), paint);
}

// Play glyph: closed triangle with a miter stroke that exercises the join limit.
[[nodiscard]] PathShape play_glyph() {
    Path path;
    path.segments.push_back(move_to(104.0f, 80.0f));
    path.segments.push_back(line_to(168.0f, 128.0f));
    path.segments.push_back(line_to(104.0f, 176.0f));
    path.segments.push_back(close_path());
    PaintStyle paint;
    paint.fill = rgba(74, 222, 128);
    paint.stroke = stroke(rgba(21, 128, 61), 3.0f, LineCap::butt, LineJoin::miter);
    return shape(std::move(path), paint);
}

// Clock: stroked-only ellipse (cubics) plus round-capped hands.
[[nodiscard]] PathShape clock_face() {
    Path path;
    append_circle(path, 196.0f, 178.0f, 26.0f);
    PaintStyle paint;
    paint.stroke = stroke(rgba(226, 232, 240), 5.0f, LineCap::butt, LineJoin::round);
    return shape(std::move(path), paint);
}

[[nodiscard]] PathShape clock_hands() {
    Path path;
    path.segments.push_back(move_to(196.0f, 178.0f));
    path.segments.push_back(line_to(196.0f, 160.0f));
    path.segments.push_back(move_to(196.0f, 178.0f));
    path.segments.push_back(line_to(214.0f, 190.0f));
    PaintStyle paint;
    paint.stroke = stroke(rgba(226, 232, 240), 3.0f, LineCap::round, LineJoin::round);
    return shape(std::move(path), paint);
}

// Favourite heart: closed cubic lobes with a round-joined outline.
[[nodiscard]] PathShape heart() {
    Path path;
    path.segments.push_back(move_to(352.0f, 160.0f));
    path.segments.push_back(cubic_to(282.0f, 120.0f, 290.0f, 50.0f, 326.0f, 50.0f));
    path.segments.push_back(cubic_to(344.0f, 50.0f, 352.0f, 66.0f, 352.0f, 80.0f));
    path.segments.push_back(cubic_to(352.0f, 66.0f, 360.0f, 50.0f, 378.0f, 50.0f));
    path.segments.push_back(cubic_to(414.0f, 50.0f, 422.0f, 120.0f, 352.0f, 160.0f));
    path.segments.push_back(close_path());
    PaintStyle paint;
    paint.fill = rgba(244, 114, 182);
    paint.stroke = stroke(rgba(190, 24, 93), 2.0f, LineCap::butt, LineJoin::round);
    return shape(std::move(path), paint);
}

// Even-odd pentagram: self-intersecting fill rule sample.
[[nodiscard]] PathShape pentagram() {
    Path path;
    append_pentagram(path, 452.0f, 190.0f, 44.0f);
    path.fill_rule = FillRule::even_odd;
    PaintStyle paint;
    paint.fill = rgba(250, 204, 21);
    paint.stroke = stroke(rgba(161, 98, 7), 2.0f, LineCap::butt, LineJoin::round);
    return shape(std::move(path), paint);
}

[[nodiscard]] PathShape bar(float x, float height, Rgba8 color) {
    Path path;
    append_rect(path, x, 470.0f - height, 30.0f, height);
    PaintStyle paint;
    paint.fill = color;
    return shape(std::move(path), paint);
}

// Trend line over the bars: open polyline with miter joins and butt caps.
[[nodiscard]] PathShape trend_line() {
    Path path;
    path.segments.push_back(move_to(291.0f, 370.0f));
    path.segments.push_back(line_to(333.0f, 320.0f));
    path.segments.push_back(line_to(375.0f, 390.0f));
    path.segments.push_back(line_to(417.0f, 280.0f));
    path.segments.push_back(line_to(459.0f, 340.0f));
    PaintStyle paint;
    paint.stroke = stroke(rgba(251, 191, 36), 4.0f, LineCap::butt, LineJoin::miter);
    return shape(std::move(path), paint);
}

// Donut: outer and inner circles with even-odd fill. With non-zero they would fill solid because
// both sub-paths run clockwise.
[[nodiscard]] PathShape donut() {
    Path path;
    append_circle(path, 108.0f, 392.0f, 74.0f);
    append_circle(path, 108.0f, 392.0f, 44.0f);
    path.fill_rule = FillRule::even_odd;
    PaintStyle paint;
    paint.fill = rgba(167, 139, 250);
    return shape(std::move(path), paint);
}

// Quadratic ribbon: an open stroke built only from quadratic segments, round caps and joins.
[[nodiscard]] PathShape quad_ribbon() {
    Path path;
    path.segments.push_back(move_to(210.0f, 492.0f));
    path.segments.push_back(quad_to(270.0f, 462.0f, 330.0f, 500.0f));
    path.segments.push_back(quad_to(392.0f, 534.0f, 452.0f, 486.0f));
    path.segments.push_back(quad_to(482.0f, 466.0f, 496.0f, 478.0f));
    PaintStyle paint;
    paint.stroke = stroke(rgba(248, 113, 113), 8.0f, LineCap::round, LineJoin::round);
    return shape(std::move(path), paint);
}

// Translucent overlay: half-covered pixels expose the straight-alpha compositing contract.
[[nodiscard]] PathShape alpha_overlay() {
    Path path;
    append_circle(path, 256.0f, 256.0f, 54.0f);
    PaintStyle paint;
    paint.fill = rgba(255, 255, 255, 64);
    return shape(std::move(path), paint);
}

} // namespace

std::vector<PathShape> build_icon_chart_scene(float width, float height) {
    std::vector<PathShape> shapes;
    shapes.reserve(15);

    shapes.push_back(app_tile());
    shapes.push_back(play_glyph());
    shapes.push_back(clock_face());
    shapes.push_back(clock_hands());
    shapes.push_back(heart());
    shapes.push_back(pentagram());

    shapes.push_back(bar(276.0f, 90.0f, rgba(45, 212, 191)));
    shapes.push_back(bar(318.0f, 140.0f, rgba(20, 184, 166)));
    shapes.push_back(bar(360.0f, 70.0f, rgba(13, 148, 136)));
    shapes.push_back(bar(402.0f, 180.0f, rgba(17, 94, 89)));
    shapes.push_back(bar(444.0f, 120.0f, rgba(94, 234, 212)));
    shapes.push_back(trend_line());

    shapes.push_back(donut());
    shapes.push_back(quad_ribbon());
    shapes.push_back(alpha_overlay());

    const float sx = width / 512.0f;
    const float sy = height / 512.0f;
    if (sx != 1.0f || sy != 1.0f) {
        for (PathShape& entry : shapes) {
            scale_path(entry.path, sx, sy);
        }
    }
    return shapes;
}

} // namespace tessera::poc::thorvg_spike
