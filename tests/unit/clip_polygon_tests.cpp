// SPDX-License-Identifier: MIT
//
// `clip-path: polygon()` (ADR-068 slice 5, issue #673): the parser, the record it produces, and the
// fill-rule coverage the CPU reference and the shader share.
//
// The fixtures use .25 vertex offsets and slopes that never put a pixel centre on an edge, so the
// expected answers do not depend on how a platform rounds the crossing computation. A polygon that
// does pass through pixel centres is the over-limit and tie cases the GPU test avoids on purpose.

#include "tessera/rendering/gpu/clip_shapes.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clip_path.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace clip = tessera::rendering::gpu::clip;
namespace detail = tessera::ui::paint::detail;
namespace rhi = tessera::rendering::rhi;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "clip_polygon: " << message << "\n";
        ++g_failures;
    }
}

constexpr rhi::Rect kBox{0.0f, 0.0f, 100.0f, 100.0f};

clip::ClipRecord polygon_record(const std::vector<clip::ClipVertex>& vertices, bool even_odd) {
    detail::ClipShape shape;
    shape.kind = detail::ClipShape::Kind::Polygon;
    shape.vertices = vertices;
    shape.even_odd = even_odd;
    return detail::to_clip_record(shape, kBox);
}

bool covered(const clip::ClipRecord& record, const std::vector<clip::ClipVertex>& pool, int x,
             int y) {
    return clip::covers_pixel(record, x, y, pool);
}

// --- Parsing -----------------------------------------------------------------------------------

void test_parse() {
    const auto triangle = detail::parse_clip_path("polygon(0 0, 100% 0, 50% 100%)", kBox);
    expect(!triangle.invalid && !triangle.unsupported &&
               triangle.shape.kind == detail::ClipShape::Kind::Polygon,
           "a polygon parses");
    expect(triangle.shape.vertices.size() == 3 && !triangle.shape.even_odd,
           "three vertices, nonzero by default");
    expect(triangle.shape.vertices[1].x == 100.0f && triangle.shape.vertices[2].x == 50.0f &&
               triangle.shape.vertices[2].y == 100.0f,
           "percentages resolve against the box width and height");

    const auto even = detail::parse_clip_path("polygon(evenodd, 0px 0px, 10px 0px, 5px 8px)", kBox);
    expect(!even.invalid && even.shape.even_odd && even.shape.vertices.size() == 3,
           "a leading fill rule is read");
    const auto nonzero = detail::parse_clip_path("polygon(nonzero, 0 0, 10px 0, 5px 8px)", kBox);
    expect(!nonzero.invalid && !nonzero.shape.even_odd, "an explicit nonzero is the default rule");

    const rhi::Rect offset{10.0f, 20.0f, 50.0f, 40.0f};
    const auto shifted = detail::parse_clip_path("polygon(0 0, 100% 0, 100% 100%)", offset);
    expect(shifted.shape.vertices[0].x == 10.0f && shifted.shape.vertices[0].y == 20.0f &&
               shifted.shape.vertices[2].x == 60.0f && shifted.shape.vertices[2].y == 60.0f,
           "the box origin is added to every vertex");

    expect(detail::parse_clip_path("polygon()", kBox).invalid, "an empty polygon is invalid");
    expect(detail::parse_clip_path("polygon(evenodd)", kBox).invalid,
           "a fill rule alone is invalid");
    expect(detail::parse_clip_path("polygon(0 0, 10px)", kBox).invalid,
           "a vertex needs two coordinates");
    expect(detail::parse_clip_path("polygon(0 0, 1px 2px 3px)", kBox).invalid,
           "a vertex with three coordinates is invalid");
    expect(detail::parse_clip_path("polygon(0 0, 10px 0,)", kBox).invalid,
           "a trailing comma is invalid");
    expect(detail::parse_clip_path("polygon(0 0, auto 0)", kBox).invalid,
           "a coordinate that is not a length is invalid");
    expect(detail::parse_clip_path("path('M0 0')", kBox).unsupported, "path() stays unsupported");
}

void test_record() {
    const auto parsed =
        detail::parse_clip_path("polygon(evenodd, 10px 20px, 90px 30px, 40px 80px)", kBox);
    const clip::ClipRecord record = detail::to_clip_record(parsed.shape, kBox);
    expect(record.kind == static_cast<std::uint32_t>(clip::RecordKind::Polygon),
           "a polygon becomes a Polygon record");
    expect(record.x == 10.0f && record.y == 20.0f && record.width == 80.0f &&
               record.height == 60.0f,
           "the record's rect is the bounding box");
    expect(record.vertex_count == 3 && (record.flags & clip::kRecordFlagEvenOdd) != 0,
           "the record carries the vertex count and the fill rule");
}

// --- Coverage ----------------------------------------------------------------------------------

void test_convex() {
    const std::vector<clip::ClipVertex> triangle{{8.25f, 8.25f}, {56.25f, 8.25f}, {32.25f, 56.25f}};
    const clip::ClipRecord record = polygon_record(triangle, false);
    expect(covered(record, triangle, 32, 20), "the triangle's interior is covered");
    expect(!covered(record, triangle, 4, 4), "a pixel outside the bounding box is not covered");
    // The bounding box's bottom-left corner region is outside the triangle: the box fallback would
    // have covered it, so this is what tells the polygon test from the box test.
    expect(!covered(record, triangle, 12, 50),
           "a bounding-box pixel outside the triangle is not covered");
    expect(!covered(record, triangle, 52, 50), "the other bounding-box corner is not covered");
    // Winding direction does not matter for nonzero.
    const std::vector<clip::ClipVertex> reversed{triangle[2], triangle[1], triangle[0]};
    expect(covered(polygon_record(reversed, false), reversed, 32, 20),
           "a clockwise and an anticlockwise polygon cover the same pixel");
}

void test_concave() {
    // An L: the notch at the right of the stem is inside the bounding box but outside the shape.
    const std::vector<clip::ClipVertex> l_shape{{10.25f, 10.25f}, {40.25f, 10.25f},
                                                {40.25f, 30.25f}, {25.25f, 30.25f},
                                                {25.25f, 50.25f}, {10.25f, 50.25f}};
    const clip::ClipRecord record = polygon_record(l_shape, false);
    expect(covered(record, l_shape, 15, 45), "the stem is covered");
    expect(covered(record, l_shape, 35, 20), "the arm is covered");
    expect(!covered(record, l_shape, 35, 45), "the notch is not covered");
    expect(!covered(record, l_shape, 30, 40), "the notch edge region is not covered");
}

std::vector<clip::ClipVertex> pentagram() {
    // Five points on a circle joined every second one: a self-intersecting star whose middle
    // pentagon winds twice.
    std::vector<clip::ClipVertex> points;
    const float centre = 32.1f;
    const float radius = 28.0f;
    constexpr int kOrder[5] = {0, 2, 4, 1, 3};
    for (const int step : kOrder) {
        const float angle = -1.5707963f + static_cast<float>(step) * 1.2566371f;
        points.push_back({centre + radius * std::cos(angle), centre + radius * std::sin(angle)});
    }
    return points;
}

void test_fill_rules() {
    const std::vector<clip::ClipVertex> star = pentagram();
    const clip::ClipRecord nonzero = polygon_record(star, false);
    const clip::ClipRecord evenodd = polygon_record(star, true);
    expect(covered(nonzero, star, 32, 32), "nonzero fills the star's middle (winding 2)");
    expect(!covered(evenodd, star, 32, 32),
           "evenodd leaves the star's middle empty (two crossings)");
    // An arm is crossed once, so both rules cover it.
    expect(covered(nonzero, star, 32, 9) && covered(evenodd, star, 32, 9),
           "both rules cover a star arm");
    expect(!covered(nonzero, star, 3, 3) && !covered(evenodd, star, 3, 3),
           "neither rule covers a corner outside the star");
}

void test_fallbacks() {
    const std::vector<clip::ClipVertex> triangle{{8.25f, 8.25f}, {56.25f, 8.25f}, {32.25f, 56.25f}};
    const clip::ClipRecord record = polygon_record(triangle, false);
    // No pool, or a pool the record's range does not fit in, is the bounding box.
    expect(clip::covers_pixel(record, 12, 50, {}), "without a pool a polygon is its bounding box");
    clip::ClipRecord past = record;
    past.first_vertex = 2;
    expect(clip::covers_pixel(past, 12, 50, triangle),
           "a range past the pool is the bounding box, not an out-of-bounds read");
}

// --- The CPU reference -------------------------------------------------------------------------

void test_canvas() {
    const std::vector<clip::ClipVertex> triangle{{8.25f, 8.25f}, {56.25f, 8.25f}, {32.25f, 56.25f}};
    const std::vector<clip::ClipRecord> records{polygon_record(triangle, false)};
    tessera::rendering::software::CpuCanvas canvas(64, 64);
    canvas.clear(rhi::rgba8(0, 0, 0));
    const tessera::rendering::software::ShapedClip shaped{records, triangle};
    canvas.fill_rect_clipped(rhi::Rect{0.0f, 0.0f, 64.0f, 64.0f}, rhi::rgba8(255, 0, 0), shaped);
    const auto red = [&canvas](std::uint32_t x, std::uint32_t y) {
        return canvas.pixels()[(static_cast<std::size_t>(y) * canvas.width() + x) * 4];
    };
    expect(red(32, 20) == 255, "the canvas paints the triangle's interior");
    expect(red(12, 50) == 0 && red(52, 50) == 0, "the canvas leaves the box corners unpainted");
}

} // namespace

int main() {
    test_parse();
    test_record();
    test_convex();
    test_concave();
    test_fill_rules();
    test_fallbacks();
    test_canvas();
    if (g_failures != 0) {
        std::cerr << "clip_polygon: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "clip_polygon: ok\n";
    return 0;
}
