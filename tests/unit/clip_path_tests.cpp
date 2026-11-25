// SPDX-License-Identifier: MIT
//
// `clip-path` basic shapes (ADR-068 slice 4, issue #672): the parser, the record it produces, and
// the coverage rule the CPU reference and the shader share.
//
// The coverage assertions are the important half. `gpu::clip::covers_pixel` is the one function the
// CPU golden and the shader's hard-test path both implement, so pinning its answers here is what
// makes a shaped-clip golden a test rather than a tolerance argument.

#include "tessera/rendering/gpu/clip_shapes.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/clip_path.hpp"

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
        std::cerr << "clip_path: " << message << "\n";
        ++g_failures;
    }
}

constexpr rhi::Rect kBox{0.0f, 0.0f, 100.0f, 100.0f};

// --- Parsing -----------------------------------------------------------------------------------

void test_none() {
    const auto parsed = detail::parse_clip_path("none", kBox);
    expect(parsed.shape.is_none() && !parsed.invalid && !parsed.unsupported,
           "`none` is the no-clip value");
    expect(detail::parse_clip_path("", kBox).shape.is_none(), "an empty value is no clip");
    expect(detail::parse_clip_path("  none  ", kBox).shape.is_none(),
           "surrounding space is ignored");
}

void test_inset() {
    const auto one = detail::parse_clip_path("inset(10px)", kBox);
    expect(one.shape.kind == detail::ClipShape::Kind::Inset, "inset() parses");
    expect(one.shape.inset_top == 10.0f && one.shape.inset_right == 10.0f &&
               one.shape.inset_bottom == 10.0f && one.shape.inset_left == 10.0f,
           "one inset value applies to all four sides");

    const auto two = detail::parse_clip_path("inset(10px 20px)", kBox);
    expect(two.shape.inset_top == 10.0f && two.shape.inset_right == 20.0f &&
               two.shape.inset_bottom == 10.0f && two.shape.inset_left == 20.0f,
           "two inset values are vertical then horizontal");

    const auto four = detail::parse_clip_path("inset(1px 2px 3px 4px)", kBox);
    expect(four.shape.inset_top == 1.0f && four.shape.inset_right == 2.0f &&
               four.shape.inset_bottom == 3.0f && four.shape.inset_left == 4.0f,
           "four inset values are top/right/bottom/left");

    // A percentage resolves against the axis it applies to: the top inset against the height, the
    // right inset against the width.
    const auto percent = detail::parse_clip_path("inset(10% 20%)", kBox);
    expect(percent.shape.inset_top == 10.0f && percent.shape.inset_right == 20.0f,
           "an inset percentage resolves against its own axis");

    const auto rounded = detail::parse_clip_path("inset(10px round 5px)", kBox);
    expect(rounded.shape.radius_tl == 5.0f && rounded.shape.radius_tr == 5.0f &&
               rounded.shape.radius_br == 5.0f && rounded.shape.radius_bl == 5.0f,
           "inset(round) parses the corner radius");

    const auto elliptical = detail::parse_clip_path("inset(10px round 5px 8px)", kBox);
    expect(elliptical.shape.radius_tl == 5.0f && elliptical.shape.radius_tr == 8.0f,
           "inset(round) expands two radii like the CSS 1-to-4 rule");
}

void test_circle_and_ellipse() {
    const auto circle = detail::parse_clip_path("circle(30px)", kBox);
    expect(circle.shape.kind == detail::ClipShape::Kind::Circle, "circle() parses");
    expect(circle.shape.radius_x == 30.0f && circle.shape.radius_y == 30.0f,
           "a circle has equal radii");
    expect(circle.shape.center_x == 50.0f && circle.shape.center_y == 50.0f,
           "a circle without `at` is centred");

    const auto positioned = detail::parse_clip_path("circle(20px at 10px 20px)", kBox);
    expect(positioned.shape.center_x == 10.0f && positioned.shape.center_y == 20.0f,
           "circle(at) places the centre");

    const auto keyword = detail::parse_clip_path("circle(20px at left top)", kBox);
    expect(keyword.shape.center_x == 0.0f && keyword.shape.center_y == 0.0f,
           "circle(at left top) is the box corner");

    const auto ellipse = detail::parse_clip_path("ellipse(40px 20px)", kBox);
    expect(ellipse.shape.kind == detail::ClipShape::Kind::Ellipse, "ellipse() parses");
    expect(ellipse.shape.radius_x == 40.0f && ellipse.shape.radius_y == 20.0f,
           "an ellipse keeps its two radii");

    // A percentage radius resolves against the box's own axis, which is the CSS rule.
    const auto percent = detail::parse_clip_path("ellipse(50% 25%)", kBox);
    expect(percent.shape.radius_x == 50.0f && percent.shape.radius_y == 25.0f,
           "an ellipse percentage resolves against its own axis");
}

void test_unsupported_and_invalid() {
    // `polygon()` is slice 5 (clip_polygon_tests.cpp). `url()` and `path()` stay unsupported:
    // reporting them as such is what lets a caller count the gap instead of silently painting
    // unclipped.
    expect(detail::parse_clip_path("url(#mask)", kBox).unsupported, "url() is unsupported");
    expect(detail::parse_clip_path("path('M0 0')", kBox).unsupported, "path() is unsupported");

    expect(detail::parse_clip_path("inset(", kBox).invalid, "a malformed inset is invalid");
    expect(detail::parse_clip_path("inset(10px 20px 30px 40px 50px)", kBox).invalid,
           "too many inset values is invalid");
    expect(detail::parse_clip_path("circle(10px 20px)", kBox).invalid,
           "a circle with two radii is invalid");
    expect(detail::parse_clip_path("inset(auto)", kBox).invalid, "an inset of `auto` is invalid");
}

// --- Records -----------------------------------------------------------------------------------

void test_records() {
    const auto inset = detail::parse_clip_path("inset(10px)", kBox);
    const clip::ClipRecord inset_record = detail::to_clip_record(inset.shape, kBox);
    expect(inset_record.kind == static_cast<std::uint32_t>(clip::RecordKind::RoundedRect),
           "an inset becomes a RoundedRect record");
    expect(inset_record.x == 10.0f && inset_record.y == 10.0f && inset_record.width == 80.0f &&
               inset_record.height == 80.0f,
           "an inset record is the box shrunk by the insets");

    const auto circle = detail::parse_clip_path("circle(30px)", kBox);
    const clip::ClipRecord circle_record = detail::to_clip_record(circle.shape, kBox);
    expect(circle_record.kind == static_cast<std::uint32_t>(clip::RecordKind::Ellipse),
           "a circle becomes an Ellipse record");
    expect(circle_record.x == 20.0f && circle_record.y == 20.0f && circle_record.width == 60.0f &&
               circle_record.height == 60.0f,
           "an ellipse record is the shape's bounding box");

    // An inset that consumes the box clips everything away rather than producing a negative rect.
    const auto consumed = detail::parse_clip_path("inset(60px)", kBox);
    const clip::ClipRecord empty = detail::to_clip_record(consumed.shape, kBox);
    expect(empty.width == 0.0f && empty.height == 0.0f,
           "an inset larger than the box becomes an empty record");
}

// --- Coverage ----------------------------------------------------------------------------------

void test_ellipse_coverage() {
    const clip::Ellipse ellipse{50.0f, 50.0f, 40.0f, 20.0f};
    expect(clip::inside_ellipse(ellipse, 50.0f, 50.0f), "the ellipse centre is inside");
    expect(clip::inside_ellipse(ellipse, 89.0f, 50.0f), "a point near the x edge is inside");
    expect(!clip::inside_ellipse(ellipse, 91.0f, 50.0f), "a point past the x edge is outside");
    expect(clip::inside_ellipse(ellipse, 50.0f, 69.0f), "a point near the y edge is inside");
    expect(!clip::inside_ellipse(ellipse, 50.0f, 71.0f), "a point past the y edge is outside");
    // The corner of the bounding box is outside the ellipse, which is the whole point of the shape.
    expect(!clip::inside_ellipse(ellipse, 88.0f, 68.0f), "the bounding-box corner is outside");
}

void test_rounded_rect_coverage() {
    clip::ClipRecord record;
    record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
    record.x = 0.0f;
    record.y = 0.0f;
    record.width = 100.0f;
    record.height = 100.0f;
    record.radii[0] = 20.0f;
    record.radii[1] = 20.0f;
    record.radii[2] = 20.0f;
    record.radii[3] = 20.0f;

    expect(clip::covers_pixel(record, 50, 50), "the centre of a rounded rect is covered");
    expect(clip::covers_pixel(record, 50, 0), "the middle of the top edge is covered");
    // The corner pixel's centre is (0.5, 0.5), which is outside the 20px corner arc.
    expect(!clip::covers_pixel(record, 0, 0), "the square corner of a rounded rect is not covered");
    expect(clip::covers_pixel(record, 20, 20), "a pixel inside the corner arc is covered");

    // A square rect covers its corner.
    clip::ClipRecord square = record;
    square.radii[0] = 0.0f;
    square.radii[1] = 0.0f;
    square.radii[2] = 0.0f;
    square.radii[3] = 0.0f;
    expect(clip::covers_pixel(square, 0, 0), "a square rect covers its corner");
}

void test_elliptical_corner_coverage() {
    // A corner with a horizontal radius of 40 and a vertical radius of 10: the pixel at (5, 5) is
    // inside that ellipse, while the same pixel would be outside a circular 40px corner.
    clip::ClipRecord record;
    record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
    record.width = 100.0f;
    record.height = 100.0f;
    record.radii[0] = 40.0f; // horizontal
    record.radii[4] = 10.0f; // vertical
    expect(clip::covers_pixel(record, 5, 5),
           "an elliptical corner covers a pixel a circular corner would not");
    // The pixel at (5, 15) is below the vertical radius, so it is in the straight part of the edge.
    expect(clip::covers_pixel(record, 5, 15), "below the corner arc the edge is straight");
    // The pixel at (35, 5) is past the horizontal radius, so it is in the straight top edge.
    expect(clip::covers_pixel(record, 35, 5), "past the corner arc the top edge is straight");
}

void test_cpu_canvas_shaped_clip() {
    // A circle clip on a full-canvas fill: the centre is painted, the corner is not. This is the
    // CPU reference a shaped-clip golden compares against.
    tessera::rendering::software::CpuCanvas canvas(64, 64);
    canvas.clear(rhi::rgba8(0, 0, 0));

    const auto circle =
        detail::parse_clip_path("circle(24px at 32px 32px)", rhi::Rect{0.0f, 0.0f, 64.0f, 64.0f});
    const clip::ClipRecord record =
        detail::to_clip_record(circle.shape, rhi::Rect{0.0f, 0.0f, 64.0f, 64.0f});
    const std::vector<clip::ClipRecord> records{record};
    const tessera::rendering::software::ShapedClip shaped{records};

    canvas.fill_rect_clipped(rhi::Rect{0.0f, 0.0f, 64.0f, 64.0f}, rhi::rgba8(255, 0, 0), shaped);

    const auto pixel = [&canvas](std::uint32_t x, std::uint32_t y) {
        const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
        return canvas.pixels()[index];
    };
    expect(pixel(32, 32) == 255, "the circle's centre is painted");
    expect(pixel(0, 0) == 0, "the canvas corner is outside the circle");
    expect(pixel(63, 63) == 0, "the opposite corner is outside the circle");
    expect(pixel(32, 8) == 255, "a pixel just inside the top of the circle is painted");
    expect(pixel(32, 2) == 0, "a pixel above the circle is not painted");
}

void test_cpu_canvas_intersects_records() {
    // Two overlapping clips intersect: a pixel must be inside both. This is the nested-clip case
    // the slice's acceptance asks for.
    tessera::rendering::software::CpuCanvas canvas(64, 64);
    canvas.clear(rhi::rgba8(0, 0, 0));

    const rhi::Rect box{0.0f, 0.0f, 64.0f, 64.0f};
    const auto left = detail::parse_clip_path("inset(0px 32px 0px 0px)", box);
    const auto right = detail::parse_clip_path("inset(0px 0px 0px 32px)", box);
    const std::vector<clip::ClipRecord> records{detail::to_clip_record(left.shape, box),
                                                detail::to_clip_record(right.shape, box)};
    const tessera::rendering::software::ShapedClip shaped{records};

    canvas.fill_rect_clipped(box, rhi::rgba8(0, 255, 0), shaped);

    const auto pixel = [&canvas](std::uint32_t x, std::uint32_t y) {
        const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
        return canvas.pixels()[index + 1];
    };
    // The two insets are disjoint (the left half and the right half), so their intersection is
    // empty and nothing is painted.
    expect(pixel(16, 32) == 0 && pixel(48, 32) == 0, "two disjoint clips intersect to nothing");
}

} // namespace

int main() {
    test_none();
    test_inset();
    test_circle_and_ellipse();
    test_unsupported_and_invalid();
    test_records();
    test_ellipse_coverage();
    test_rounded_rect_coverage();
    test_elliptical_corner_coverage();
    test_cpu_canvas_shaped_clip();
    test_cpu_canvas_intersects_records();
    if (g_failures != 0) {
        std::cerr << "clip_path: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "clip_path: ok\n";
    return 0;
}
