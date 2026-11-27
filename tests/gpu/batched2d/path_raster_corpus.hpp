// SPDX-License-Identifier: MIT
//
// The committed corpus of the GPU path rasterizer's comparison and golden tests (issue #685,
// ADR-071): every fill rule, cap, join, miter-limit fallback, dash shape and degenerate case, as
// small deterministic scenes built in code. No scene is larger than ${kCorpusSize} px square. Both
// tests/gpu/batched2d/path_raster_thorvg_compare_tests.cpp (against the ThorVG reference) and
// path_raster_golden_tests.cpp (against the lavapipe PNGs) iterate this table, so a scene added
// here is covered by both and needs an entry in the comparison tolerance table and a golden.
//
// Coordinates are quantized to 1/64 px so the trigonometry of a generated polygon rounds the same
// on every libm. Scenes avoid axis-aligned rectangles unless a scene is about exactly that,
// because ThorVG snaps those to whole pixels and the GPU path does not (ADR-071).

#pragma once

#include "tessera/rendering/path/gpu_path_scene.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tessera::path_corpus {

namespace path = tessera::rendering::path;

inline constexpr std::uint32_t kCorpusSize = 96;

struct Pt {
    float x = 0.0f;
    float y = 0.0f;
};
using Pts = std::vector<Pt>;

struct Scene {
    std::string name;
    path::GpuPathScene gpu;
    bool expect_blank = false; // nothing may land on the target
};

[[nodiscard]] inline float quantize(double v) {
    return static_cast<float>(std::round(v * 64.0) / 64.0);
}

[[nodiscard]] inline Pts polygon(double cx, double cy, double radius, int sides,
                                 double start_degrees) {
    Pts points;
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < sides; ++i) {
        const double a = (start_degrees + 360.0 * i / sides) * kPi / 180.0;
        points.push_back({quantize(cx + radius * std::cos(a)), quantize(cy + radius * std::sin(a))});
    }
    return points;
}

// The five points of a pentagram in drawing order: every second vertex of a pentagon, so the
// outline crosses itself and the center is wound twice.
[[nodiscard]] inline Pts pentagram(double cx, double cy, double radius) {
    const Pts pentagon = polygon(cx, cy, radius, 5, -90.0);
    Pts points;
    for (std::size_t i = 0; i < 5; ++i) {
        points.push_back(pentagon[(i * 2) % 5]);
    }
    return points;
}

[[nodiscard]] inline Pts rotated(const Pts& points, double cx, double cy, double degrees) {
    constexpr double kPi = 3.14159265358979323846;
    const double c = std::cos(degrees * kPi / 180.0);
    const double s = std::sin(degrees * kPi / 180.0);
    Pts result;
    for (const Pt& p : points) {
        const double dx = static_cast<double>(p.x) - cx;
        const double dy = static_cast<double>(p.y) - cy;
        result.push_back({quantize(cx + dx * c - dy * s), quantize(cy + dx * s + dy * c)});
    }
    return result;
}

[[nodiscard]] inline Pts reversed(Pts points) {
    Pts result(points.rbegin(), points.rend());
    return result;
}

inline void append_edges(std::vector<float>& out, const Pts& points, bool closed) {
    const std::size_t count = closed ? points.size() : points.size() - 1;
    for (std::size_t i = 0; i < count; ++i) {
        const Pt& from = points[i];
        const Pt& to = points[(i + 1) % points.size()];
        out.insert(out.end(), {from.x, from.y, to.x, to.y});
    }
}

[[nodiscard]] inline path::GpuPathShape fill(std::initializer_list<Pts> contours, path::Rgba8 color,
                                             path::GpuFillRule rule = path::GpuFillRule::non_zero) {
    path::GpuPathShape shape;
    shape.rgba = path::pack_rgba(color);
    shape.rule = rule;
    for (const Pts& contour : contours) {
        append_edges(shape.contours, contour, true);
        shape.contour_sizes.push_back(static_cast<std::uint32_t>(contour.size()));
        shape.contour_open.push_back(false);
    }
    return shape;
}

struct Line {
    path::Rgba8 color;
    float width = 8.0f;
    path::GpuJoin join = path::GpuJoin::round;
    path::GpuCap cap = path::GpuCap::butt;
    float miter_limit = 4.0f;
    std::vector<float> dash;
    float dash_offset = 0.0f;
    bool closed = false;
};

[[nodiscard]] inline path::GpuPathShape stroke(const Pts& points, const Line& line) {
    path::GpuPathShape shape;
    shape.rgba = path::pack_rgba(line.color);
    append_edges(shape.contours, points, line.closed);
    shape.half_width = line.width * 0.5f;
    shape.open = !line.closed;
    shape.join = line.join;
    shape.cap = line.cap;
    shape.miter_limit = line.miter_limit;
    shape.dash = path::normalized_dash_pattern(line.dash);
    shape.dash_offset = line.dash_offset;
    return shape;
}

[[nodiscard]] inline Scene scene(std::string name, std::vector<path::GpuPathShape> shapes,
                                 bool expect_blank = false) {
    Scene result;
    result.name = std::move(name);
    result.gpu.shapes = std::move(shapes);
    result.expect_blank = expect_blank;
    return result;
}

constexpr path::Rgba8 kRed{220, 40, 40, 255};
constexpr path::Rgba8 kBlue{40, 80, 230, 255};
constexpr path::Rgba8 kGreen{30, 160, 80, 255};
constexpr path::Rgba8 kOrange{240, 140, 30, 255};

inline void add_fill_rules(std::vector<Scene>& out) {
    const Pts star = pentagram(48, 50, 42);
    out.push_back(scene("fill_star_non_zero", {fill({star}, kRed)}));
    out.push_back(scene("fill_star_even_odd",
                        {fill({star}, kRed, path::GpuFillRule::even_odd)}));
    // Two nested squares (turned off the pixel grid): the same winding fills the middle under
    // non_zero and punches a hole under even_odd; opposite windings punch it under both.
    const Pts outer = rotated(polygon(48, 48, 42, 4, 45.0), 48, 48, 12.0);
    const Pts inner = rotated(polygon(48, 48, 20, 4, 45.0), 48, 48, 12.0);
    out.push_back(scene("fill_nested_same_non_zero", {fill({outer, inner}, kBlue)}));
    out.push_back(scene("fill_nested_same_even_odd",
                        {fill({outer, inner}, kBlue, path::GpuFillRule::even_odd)}));
    out.push_back(scene("fill_nested_opposite_non_zero", {fill({outer, reversed(inner)}, kBlue)}));
    out.push_back(scene("fill_circle_alpha_overlap",
                        {fill({polygon(36, 48, 30, 64, 0.0)}, {220, 40, 40, 170}),
                         fill({polygon(60, 48, 30, 64, 0.0)}, {40, 80, 230, 170})}));
    // An axis-aligned rectangle on fractional edges: ThorVG snaps it to whole pixels, the GPU
    // path keeps the coverage, so the two differ here by design.
    out.push_back(scene("fill_rect_fractional",
                        {fill({{{10.5f, 12.25f}, {80.75f, 12.25f}, {80.75f, 70.5f}, {10.5f, 70.5f}}},
                              kGreen)}));
}

inline void add_caps_and_joins(std::vector<Scene>& out) {
    const Pts zigzag{{14, 70}, {40, 20}, {56, 60}, {82, 26}};
    const Pts bar{{14, 86.5f}, {82, 86.5f}};
    for (const auto& [name, cap] : {std::pair{"stroke_cap_butt", path::GpuCap::butt},
                                    std::pair{"stroke_cap_round", path::GpuCap::round},
                                    std::pair{"stroke_cap_square", path::GpuCap::square}}) {
        Line line{kBlue};
        line.width = 10.0f;
        line.cap = cap;
        out.push_back(scene(name, {stroke(zigzag, line), stroke(bar, line)}));
    }
    // An acute zigzag (about 35 degrees, miter ratio about 3.3 against the default limit of 4)
    // above a right angle and an obtuse bend.
    const Pts acute{{8, 46}, {28, 8}, {48, 46}, {68, 8}, {88, 46}};
    const Pts right{{10, 90}, {10, 58}, {42, 58}};
    const Pts obtuse{{54, 90}, {70, 60}, {90, 82}};
    for (const auto& [name, join] : {std::pair{"stroke_join_miter", path::GpuJoin::miter},
                                     std::pair{"stroke_join_bevel", path::GpuJoin::bevel},
                                     std::pair{"stroke_join_round", path::GpuJoin::round}}) {
        Line line{kOrange};
        line.width = 8.0f;
        line.join = join;
        out.push_back(
            scene(name, {stroke(acute, line), stroke(right, line), stroke(obtuse, line)}));
    }
    // The same acute zigzag with a miter limit of 2 (ratio 3.3 exceeds it: bevel fallback) and a
    // 12 degree spike against the default limit.
    Line limited{kOrange};
    limited.width = 8.0f;
    limited.join = path::GpuJoin::miter;
    limited.miter_limit = 2.0f;
    Line spike = limited;
    spike.miter_limit = 4.0f;
    out.push_back(scene("stroke_miter_exceeds_limit",
                        {stroke(acute, limited), stroke({{8, 90}, {80, 62}, {8, 74}}, spike)}));
    const Pts triangle = rotated(polygon(48, 50, 34, 3, -90.0), 48, 50, 8.0);
    Line closed{kGreen};
    closed.width = 9.0f;
    closed.join = path::GpuJoin::miter;
    closed.closed = true;
    out.push_back(scene("stroke_closed_triangle_miter", {stroke(triangle, closed)}));
    Line open = closed;
    open.closed = false;
    out.push_back(scene("stroke_open_triangle_butt", {stroke(triangle, open)}));
}

inline void add_dashes(std::vector<Scene>& out) {
    Line lines{kBlue};
    lines.width = 6.0f;
    lines.dash = {10.0f, 6.0f};
    out.push_back(scene("dash_even_butt", {stroke({{8, 16}, {88, 16}}, lines),
                                           stroke({{8, 34}, {88, 70}}, lines),
                                           stroke({{8, 88.5f}, {88, 88.5f}}, lines)}));
    Line odd{kRed};
    odd.width = 6.0f;
    odd.cap = path::GpuCap::round;
    odd.dash = {8.0f, 3.0f, 4.0f}; // odd: repeats to {8,3,4,8,3,4}
    odd.dash_offset = 5.0f;
    out.push_back(scene("dash_odd_offset_round", {stroke({{10, 80}, {30, 16}, {58, 70}, {86, 18}},
                                                         odd)}));
    Line pentagon{kGreen};
    pentagon.width = 5.0f;
    pentagon.join = path::GpuJoin::miter;
    pentagon.dash = {7.0f, 5.0f};
    pentagon.closed = true;
    out.push_back(scene("dash_closed_polygon_butt",
                        {stroke(polygon(48, 50, 38, 5, -90.0), pentagon)}));
    Line circle{kOrange};
    circle.width = 7.0f;
    circle.cap = path::GpuCap::square;
    circle.dash = {12.0f, 8.0f};
    circle.dash_offset = 6.0f;
    circle.closed = true;
    out.push_back(scene("dash_curve_square", {stroke(polygon(48, 48, 36, 48, 0.0), circle)}));
}

inline void add_degenerate(std::vector<Scene>& out) {
    Line miter{kBlue};
    miter.width = 8.0f;
    miter.join = path::GpuJoin::miter;
    out.push_back(scene("degenerate_zero_length_mid",
                        {stroke({{12, 70}, {48, 20}, {48, 20}, {84, 70}}, miter)}));
    // A single zero-length segment: only a round or square cap has anything to draw (SVG).
    Line dot{kRed};
    dot.width = 14.0f;
    dot.cap = path::GpuCap::round;
    Line square_dot = dot;
    square_dot.cap = path::GpuCap::square;
    Line butt_dot = dot;
    butt_dot.cap = path::GpuCap::butt;
    out.push_back(scene("degenerate_zero_length_dot",
                        {stroke({{24, 24}, {24, 24}}, dot), stroke({{64, 40}, {64, 40}}, square_dot),
                         stroke({{40, 74}, {40, 74}}, butt_dot)}));
    Line through{kGreen};
    through.width = 6.0f;
    through.join = path::GpuJoin::miter;
    const Pts triangle = rotated({{14, 80}, {52, 80}, {90, 80}, {52, 22}, {33, 51}}, 48, 50, 9.0);
    out.push_back(scene("degenerate_collinear",
                        {fill({triangle}, {240, 140, 30, 255}),
                         stroke(rotated({{10, 10}, {30, 20}, {50, 30}, {70, 40}}, 48, 50, 0.0),
                                through)}));
    Line hairline{kBlue};
    hairline.width = 0.25f;
    out.push_back(scene("degenerate_thin",
                        {fill({{{6, 10}, {90, 40}, {6, 10.3f}}}, kRed),
                         stroke({{6, 60}, {90, 80}}, hairline)}));
    out.push_back(scene("degenerate_empty", {}, true));
    Line edge{kBlue};
    edge.width = 6.0f;
    out.push_back(scene("degenerate_outside_target",
                        {fill({polygon(140, 40, 20, 12, 0.0)}, kRed),
                         fill({polygon(-40, 50, 20, 12, 0.0)}, kRed),
                         stroke({{-60, 20}, {-10, 90}}, edge), stroke({{20, 160}, {80, 220}}, edge)},
                        true));
    Line cut{kGreen};
    cut.width = 9.0f;
    cut.cap = path::GpuCap::round;
    out.push_back(scene("degenerate_clipped_by_edge",
                        {fill({polygon(2, 2, 34, 48, 0.0)}, kRed),
                         fill({polygon(90, 98, 30, 48, 0.0)}, kBlue),
                         stroke({{40, 48}, {140, 70}}, cut)}));
}

[[nodiscard]] inline std::vector<Scene> make_corpus() {
    std::vector<Scene> corpus;
    add_fill_rules(corpus);
    add_caps_and_joins(corpus);
    add_dashes(corpus);
    add_degenerate(corpus);
    return corpus;
}

} // namespace tessera::path_corpus
