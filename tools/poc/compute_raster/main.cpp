// SPDX-License-Identifier: MIT
//
// ADR-071 spike (issue #683): a GPU compute path rasterizer (fill only, scanline-binned, exact
// pixel area) measured on the two 4K scenes of cpu_to_gpu_4k_measure, and compared with the ThorVG
// CPU raster of the same scene. A measurement tool, not a test: it prints GPU time per frame
// (timestamps around the whole submission), the CPU time to record and submit it, the bytes
// uploaded per frame after the first (zero: the scene is static), and the pixel difference from
// ThorVG.
//
// Strokes come in two forms so they can be compared: the ring form (the CPU builds the outer
// contour at r + w/2 and the inner one reversed at r - w/2, which only works for closed blobs) and
// the GPU stroker (the CPU uploads the flattened centerline and a half width; a compute pass
// expands every segment into a body quad plus round join wedges and caps, so open polylines work
// too). Flattening the curve stays on the CPU. Miter/bevel joins, butt/square caps and dashes are
// not implemented. Transforms are not implemented either: an animated frame would change a
// push-constant matrix, not an upload, which this tool does not exercise.

#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace path = tessera::rendering::path;
using Clock = std::chrono::steady_clock;

alignas(4) const std::uint32_t kClearSpv[] =
#include "raster_clear_spv.h"
    ;
alignas(4) const std::uint32_t kStrokeSpv[] =
#include "raster_stroke_spv.h"
    ;
alignas(4) const std::uint32_t kBinSpv[] =
#include "raster_bin_spv.h"
    ;
alignas(4) const std::uint32_t kFineSpv[] =
#include "raster_fine_spv.h"
    ;

// ---- scenes (the same geometry as cpu_to_gpu_4k_measure)
// ------------------------------------------

[[nodiscard]] path::Path rect_path(float x0, float y0, float x1, float y1) {
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {x0, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x0, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

[[nodiscard]] path::Path blob_path(float cx, float cy, float r) {
    constexpr float k = 0.5522847498f;
    const float c = r * k;
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {cx + r, cy}, {}, {}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx + r, cy + c}, {cx + c, cy + r}, {cx, cy + r}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx - c, cy + r}, {cx - r, cy + c}, {cx - r, cy}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx - r, cy - c}, {cx - c, cy - r}, {cx, cy - r}});
    result.segments.push_back(
        {path::SegmentKind::cubic_to, {cx + c, cy - r}, {cx + r, cy - c}, {cx + r, cy}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

[[nodiscard]] path::PathShape filled(path::Path geometry, path::Rgba8 color) {
    path::PathShape shape;
    shape.path = std::move(geometry);
    shape.paint.fill = color;
    return shape;
}

struct LineStyle {
    path::LineJoin join = path::LineJoin::round;
    path::LineCap cap = path::LineCap::round;
    float miter_limit = 4.0f;
    bool closed = false;
    std::vector<float> dash; // as written in SVG: an odd count repeats to make an even one
    float dash_offset = 0.0f;
};

[[nodiscard]] std::vector<float> normalized_dash(std::vector<float> pattern) {
    if (pattern.size() % 2 == 1) {
        const std::size_t count = pattern.size();
        for (std::size_t i = 0; i < count; ++i) {
            pattern.push_back(pattern[i]);
        }
    }
    return pattern;
}

[[nodiscard]] path::PathShape stroked(path::Path geometry, path::Rgba8 color, float width,
                                      const LineStyle& style = {}) {
    path::PathShape shape;
    shape.path = std::move(geometry);
    path::StrokeStyle stroke;
    stroke.color = color;
    stroke.width = width;
    stroke.cap = style.cap;
    stroke.join = style.join;
    stroke.miter_limit = style.miter_limit;
    shape.paint.stroke = stroke;
    return shape;
}

// The ThorVG-side scene description plus what the GPU needs to draw the same thing.
struct GpuShape {
    std::vector<float> contours; // x0 y0 x1 y1 per segment, closed contours back to back
    std::uint32_t rgba = 0;
    std::uint32_t rule = 0; // 0 non-zero, 1 even-odd
    // > 0: `contours` is a stroke centerline (one segment per edge, in order) with this half width,
    // expanded on the GPU; `open` puts a round cap at the end (the start has no previous vertex).
    float half_width = 0.0f;
    bool open = false;
    std::vector<float> dash; // normalized on/off lengths (even count, at most 8); empty = solid
    float dash_offset = 0.0f;
    std::uint32_t join = 0; // 0 round, 1 bevel, 2 miter
    std::uint32_t cap = 0;  // 0 round, 1 butt, 2 square
    float miter_limit = 4.0f;
};

[[nodiscard]] std::uint32_t join_code(path::LineJoin join) {
    return join == path::LineJoin::round ? 0U : join == path::LineJoin::bevel ? 1U : 2U;
}

[[nodiscard]] std::uint32_t cap_code(path::LineCap cap) {
    return cap == path::LineCap::round ? 0U : cap == path::LineCap::butt ? 1U : 2U;
}

// How the scene builders emit a stroke: false = ring contours on the CPU, true = GPU stroker.
bool g_gpu_stroke = true;
// Drops the strokes from the sparse and dense scenes (for the affine comparison, see Motion).
bool g_fills_only = false;

struct Affine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f; // x' = a x + c y + e
};

[[nodiscard]] path::PointF apply(const Affine& m, path::PointF p) {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}

struct SceneDescription {
    std::vector<path::PathShape> reference;
    std::vector<GpuShape> gpu;
};

[[nodiscard]] std::uint32_t pack(path::Rgba8 c) {
    return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
           (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
}

// Flattens the four-cubic blob into line segments; `reverse` flips the winding.
void flatten_blob(std::vector<float>& out, float cx, float cy, float r, bool reverse) {
    constexpr float k = 0.5522847498f;
    const float c = r * k;
    const int per_cubic = std::clamp(static_cast<int>(std::ceil(r / 12.0f)), 16, 128);
    const float cubics[4][8] = {
        {cx + r, cy, cx + r, cy + c, cx + c, cy + r, cx, cy + r},
        {cx, cy + r, cx - c, cy + r, cx - r, cy + c, cx - r, cy},
        {cx - r, cy, cx - r, cy - c, cx - c, cy - r, cx, cy - r},
        {cx, cy - r, cx + c, cy - r, cx + r, cy - c, cx + r, cy},
    };
    std::vector<float> points;
    for (const auto& q : cubics) {
        for (int i = 0; i < per_cubic; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(per_cubic);
            const float u = 1.0f - t;
            const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            points.push_back(b0 * q[0] + b1 * q[2] + b2 * q[4] + b3 * q[6]);
            points.push_back(b0 * q[1] + b1 * q[3] + b2 * q[5] + b3 * q[7]);
        }
    }
    const std::size_t count = points.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t j = (i + 1) % count;
        const float x0 = points[i * 2], y0 = points[i * 2 + 1];
        const float x1 = points[j * 2], y1 = points[j * 2 + 1];
        if (reverse) {
            out.insert(out.end(), {x1, y1, x0, y0});
        } else {
            out.insert(out.end(), {x0, y0, x1, y1});
        }
    }
}

void add_fill_blob(SceneDescription& scene, float cx, float cy, float r, path::Rgba8 color) {
    scene.reference.push_back(filled(blob_path(cx, cy, r), color));
    GpuShape shape;
    shape.rgba = pack(color);
    flatten_blob(shape.contours, cx, cy, r, false);
    scene.gpu.push_back(std::move(shape));
}

void add_stroke_ring(SceneDescription& scene, float cx, float cy, float r, path::Rgba8 color,
                     float width) {
    if (g_fills_only) {
        return;
    }
    scene.reference.push_back(stroked(blob_path(cx, cy, r), color, width));
    GpuShape shape;
    shape.rgba = pack(color);
    if (g_gpu_stroke) {
        flatten_blob(shape.contours, cx, cy, r, false);
        shape.half_width = width * 0.5f;
    } else {
        flatten_blob(shape.contours, cx, cy, r + width * 0.5f, false);
        flatten_blob(shape.contours, cx, cy, r - width * 0.5f, true);
    }
    scene.gpu.push_back(std::move(shape));
}

struct Pt {
    float x, y;
};

// The CPU dasher the GPU is compared against: ThorVG's seam has no dashes, so the reference scene
// is the same dashes cut on the CPU, each one an open polyline stroked by ThorVG with the stroke's
// own cap and join. The pattern starts at the contour start (offset shifts it) and does not wrap
// into a join at a closed contour's end, as in SVG.
[[nodiscard]] path::Path dashed_path(const GpuShape& shape, float offset) {
    path::Path result;
    const std::vector<float>& pattern = shape.dash;
    double period = 0.0;
    for (const float v : pattern) {
        period += v;
    }
    std::size_t k = 0;
    double phase = std::fmod(static_cast<double>(offset), period);
    if (phase < 0.0) {
        phase += period;
    }
    double c0 = 0.0;
    while (k < pattern.size() && phase >= c0 + pattern[k]) {
        c0 += pattern[k++];
    }
    if (k >= pattern.size()) {
        k = 0;
        c0 = 0.0;
        phase = 0.0;
    }
    double remaining = c0 + pattern[k] - phase; // left in the current entry
    std::vector<Pt> run;
    const auto flush = [&] {
        if (run.size() >= 2) {
            result.segments.push_back({path::SegmentKind::move_to, {run[0].x, run[0].y}, {}, {}});
            for (std::size_t i = 1; i < run.size(); ++i) {
                result.segments.push_back(
                    {path::SegmentKind::line_to, {run[i].x, run[i].y}, {}, {}});
            }
        }
        run.clear();
    };
    for (std::size_t i = 0; i + 3 < shape.contours.size(); i += 4) {
        const double x0 = shape.contours[i], y0 = shape.contours[i + 1];
        const double dx = shape.contours[i + 2] - x0, dy = shape.contours[i + 3] - y0;
        const double length = std::sqrt(dx * dx + dy * dy);
        double done = 0.0;
        while (done < length - 1e-9) {
            const double step = std::min(remaining, length - done);
            const bool on = k % 2 == 0;
            if (on && step > 0.0) {
                if (run.empty()) {
                    run.push_back({static_cast<float>(x0 + dx * done / length),
                                   static_cast<float>(y0 + dy * done / length)});
                }
                run.push_back({static_cast<float>(x0 + dx * (done + step) / length),
                               static_cast<float>(y0 + dy * (done + step) / length)});
            }
            done += step;
            remaining -= step;
            if (remaining <= 1e-9) {
                if (on) {
                    flush();
                }
                k = (k + 1) % pattern.size();
                remaining = pattern[k];
            }
        }
    }
    flush(); // the contour ends: a dash still running stops here
    return result;
}

// A polyline (open, or closed back to its first point) with any join and cap: the shapes a ring
// cannot express.
void add_polyline(SceneDescription& scene, const std::vector<Pt>& points, path::Rgba8 color,
                  float width, const LineStyle& style = {}) {
    path::Path geometry;
    geometry.segments.push_back({path::SegmentKind::move_to, {points[0].x, points[0].y}, {}, {}});
    for (std::size_t i = 1; i < points.size(); ++i) {
        geometry.segments.push_back(
            {path::SegmentKind::line_to, {points[i].x, points[i].y}, {}, {}});
    }
    if (style.closed) {
        geometry.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    }
    GpuShape shape;
    shape.rgba = pack(color);
    const std::size_t edges = style.closed ? points.size() : points.size() - 1;
    for (std::size_t i = 0; i < edges; ++i) {
        const Pt& from = points[i];
        const Pt& to = points[(i + 1) % points.size()];
        shape.contours.insert(shape.contours.end(), {from.x, from.y, to.x, to.y});
    }
    shape.half_width = width * 0.5f;
    shape.open = !style.closed;
    shape.join = join_code(style.join);
    shape.cap = cap_code(style.cap);
    shape.miter_limit = style.miter_limit;
    shape.dash = normalized_dash(style.dash);
    shape.dash_offset = style.dash_offset;
    if (!shape.dash.empty()) {
        geometry = dashed_path(shape, shape.dash_offset);
    }
    scene.reference.push_back(stroked(std::move(geometry), color, width, style));
    scene.gpu.push_back(std::move(shape));
}

// A dashed circle-like blob: curved, so the dashes cross many short flattened segments.
void add_dashed_blob(SceneDescription& scene, float cx, float cy, float r, path::Rgba8 color,
                     float width, std::vector<float> dash, float offset, path::LineCap cap) {
    GpuShape shape;
    shape.rgba = pack(color);
    flatten_blob(shape.contours, cx, cy, r, false);
    shape.half_width = width * 0.5f;
    shape.cap = cap_code(cap);
    shape.dash = normalized_dash(std::move(dash));
    shape.dash_offset = offset;
    LineStyle style;
    style.cap = cap;
    style.closed = true;
    scene.reference.push_back(stroked(dashed_path(shape, offset), color, width, style));
    scene.gpu.push_back(std::move(shape));
}

// Dashes on lines, a zigzag, a closed polygon and a circle, with the patterns that matter: even,
// odd (which repeats), several entries, a fine one, an offset, and each cap.
[[nodiscard]] SceneDescription dash_scene() {
    SceneDescription scene;
    const LineStyle butt{path::LineJoin::round, path::LineCap::butt, 4.0f, false, {24, 12}, 0.0f};
    const LineStyle square{
        path::LineJoin::miter, path::LineCap::square, 4.0f, false, {30, 10}, 0.0f};
    const LineStyle round_caps{
        path::LineJoin::round, path::LineCap::round, 4.0f, false, {1, 20}, 0.0f};
    const LineStyle odd{path::LineJoin::bevel, path::LineCap::butt, 4.0f, false, {14}, 5.0f};
    const LineStyle thin{path::LineJoin::round, path::LineCap::butt, 4.0f, false, {6, 3}, 0.0f};
    add_polyline(scene, {{200, 200}, {3600, 260}}, {40, 160, 220, 255}, 14.0f, butt);
    add_polyline(scene, {{300, 500}, {900, 620}, {400, 900}, {1300, 960}, {1500, 520}},
                 {220, 80, 60, 255}, 22.0f, square);
    add_polyline(
        scene, {{1900, 500}, {2600, 560}, {2200, 1100}}, {240, 200, 60, 255}, 16.0f,
        LineStyle{path::LineJoin::round, path::LineCap::round, 4.0f, true, {20, 8, 4, 8}, 0.0f});
    add_polyline(scene, {{3000, 500}, {3600, 900}, {3000, 1300}}, {120, 220, 120, 255}, 12.0f, odd);
    add_polyline(scene, {{300, 1200}, {1500, 1500}, {600, 1800}}, {200, 120, 220, 255}, 18.0f,
                 round_caps);
    add_polyline(scene, {{1800, 1500}, {3400, 1560}, {2200, 2000}}, {240, 240, 240, 255}, 3.0f,
                 thin);
    add_dashed_blob(scene, 3000.0f, 1700.0f, 300.0f, {240, 120, 40, 255}, 16.0f, {18, 10}, 7.0f,
                    path::LineCap::round);
    return scene;
}

[[nodiscard]] SceneDescription polyline_scene() {
    SceneDescription scene;
    // Sharp turns of every kind (acute, right angle, obtuse, a reversal), thick and thin.
    add_polyline(scene, {{300, 300}, {900, 380}, {500, 700}, {1200, 760}, {1250, 400}},
                 {40, 160, 220, 255}, 24.0f);
    add_polyline(scene, {{1700, 300}, {2400, 300}, {2400, 900}, {1800, 900}, {1800, 320}},
                 {220, 80, 60, 230}, 40.0f);
    add_polyline(scene, {{3000, 300}, {3600, 700}, {3000, 1100}, {3500, 1500}, {3100, 1900}},
                 {240, 200, 60, 255}, 9.0f);
    add_polyline(scene, {{600, 1300}, {1500, 1300}, {700, 1320}}, {120, 220, 120, 255}, 18.0f);
    return scene;
}

// The same kind of geometry with miter and bevel joins and butt and square caps, including a closed
// polygon (whose first vertex also needs a join) and turns sharp enough to hit the miter limit.
[[nodiscard]] SceneDescription join_scene() {
    SceneDescription scene;
    const std::vector<Pt> zigzag = {{0, 0}, {300, 40}, {90, 260}, {520, 300}, {560, 20}};
    const auto shifted = [&](float dx, float dy) {
        std::vector<Pt> out;
        for (const Pt& p : zigzag) {
            out.push_back({p.x + dx, p.y + dy});
        }
        return out;
    };
    const LineStyle miter{path::LineJoin::miter, path::LineCap::butt, 4.0f, false, {}, 0.0f};
    const LineStyle miter_low{path::LineJoin::miter, path::LineCap::square, 1.2f, false, {}, 0.0f};
    const LineStyle bevel{path::LineJoin::bevel, path::LineCap::butt, 4.0f, false, {}, 0.0f};
    const LineStyle bevel_sq{path::LineJoin::bevel, path::LineCap::square, 4.0f, false, {}, 0.0f};
    add_polyline(scene, shifted(200, 200), {40, 160, 220, 255}, 28.0f, miter);
    add_polyline(scene, shifted(1000, 200), {220, 80, 60, 255}, 28.0f, miter_low);
    add_polyline(scene, shifted(1800, 200), {240, 200, 60, 255}, 28.0f, bevel);
    add_polyline(scene, shifted(2600, 200), {120, 220, 120, 255}, 28.0f, bevel_sq);
    add_polyline(scene, shifted(200, 800), {200, 120, 220, 255}, 10.0f, miter);
    add_polyline(scene, shifted(1000, 800), {200, 120, 220, 255}, 10.0f, bevel);
    // Closed: a triangle and a thin spike, miter and bevel.
    const LineStyle closed_miter{path::LineJoin::miter, path::LineCap::butt, 4.0f, true, {}, 0.0f};
    const LineStyle closed_bevel{path::LineJoin::bevel, path::LineCap::butt, 4.0f, true, {}, 0.0f};
    add_polyline(scene, {{1900, 900}, {2500, 960}, {2100, 1500}}, {40, 160, 220, 255}, 30.0f,
                 closed_miter);
    add_polyline(scene, {{2800, 900}, {3500, 960}, {3100, 1500}}, {220, 80, 60, 255}, 30.0f,
                 closed_bevel);
    add_polyline(scene, {{300, 1500}, {1500, 1560}, {320, 1640}}, {240, 200, 60, 255}, 16.0f,
                 closed_miter);
    return scene;
}

[[nodiscard]] SceneDescription sparse_scene() {
    SceneDescription scene;
    for (int i = 0; i < 15; ++i) {
        const float cx = 300.0f + static_cast<float>(i % 5) * 700.0f;
        const float cy = 400.0f + static_cast<float>(i / 5) * 650.0f;
        add_fill_blob(scene, cx, cy, 120.0f, {40, 160, 220, 255});
        add_stroke_ring(scene, cx, cy, 150.0f, {220, 80, 60, 255}, 8.0f);
    }
    return scene;
}

[[nodiscard]] SceneDescription dense_scene(float width, float height) {
    SceneDescription scene;
    const path::Rgba8 background{20, 24, 32, 255};
    scene.reference.push_back(filled(rect_path(0, 0, width, height), background));
    GpuShape rect;
    rect.rgba = pack(background);
    rect.contours = {0,     0,      width, 0,      width, 0,      width, height,
                     width, height, 0,     height, 0,     height, 0,     0};
    scene.gpu.push_back(std::move(rect));
    for (int i = 0; i < 6; ++i) {
        const float f = static_cast<float>(i);
        add_fill_blob(scene, 1920.0f + f * 40.0f, 1080.0f, 1000.0f - f * 120.0f,
                      {static_cast<std::uint8_t>(40 + i * 30), 120, 200, 200});
        add_stroke_ring(scene, 1920.0f, 1080.0f + f * 30.0f, 1050.0f - f * 100.0f,
                        {240, 200, 60, 220}, 14.0f);
    }
    return scene;
}

// ---- GPU data
// ------------------------------------------------------------------------------------

struct Encoded {
    std::vector<std::uint32_t> segs;    // fill edges
    std::vector<std::uint32_t> strokes; // 16 words per stroke record (a segment, or a chunk of one)
    std::vector<std::uint32_t> dashes;  // 12 words per path: count, offset, period, -, 8 lengths
    std::vector<float> dash_base;       // each path's own offset, before any animation
    std::vector<std::uint32_t> paths;   // 16 words per path, rewritten each animated frame
    std::vector<std::array<float, 4>> local_boxes; // min x, min y, max x, max y in path space
    std::vector<std::uint32_t> rules;
    std::vector<std::uint32_t> colors;
    std::uint32_t seg_count = 0; // fill edges + GPU-expanded stroke edges (bin threads)
    std::uint32_t fill_count = 0;
    std::uint32_t stroke_count = 0;
    std::uint32_t stroke_edges = 0; // expanded edge slots over every record, pieces included
    std::uint32_t arc_edges = 0;
    std::uint32_t path_count = 0;
    std::uint64_t row_nodes = 0; // upper bound on bin nodes: segment rows summed
};

[[nodiscard]] std::uint32_t bits(float value) {
    std::uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

// Arc edges per quarter-turn-ish rim: enough that the inscribed polygon stays within ~0.05 px of
// the circle at the widest stroke of the scene.
[[nodiscard]] std::uint32_t arc_edges_for(const SceneDescription& scene, float max_scale) {
    float widest = 0.0f;
    for (const GpuShape& shape : scene.gpu) {
        widest =
            std::max(widest, shape.half_width * max_scale); // a rim scaled up shows its corners
    }
    if (widest <= 0.0f) {
        return 0;
    }
    const float step = std::acos(std::max(0.0f, 1.0f - 0.05f / widest)); // max angle per edge
    const auto edges = static_cast<std::uint32_t>(std::ceil(3.14159265f / std::max(step, 0.01f)));
    return std::clamp<std::uint32_t>(edges, 8, 64);
}

// Rewrites the per-path records for one frame: the affine, and the device-space box that bounds the
// transformed local box (the fine pass skips a pixel outside it). An empty `matrices` is identity.
void write_paths(Encoded& encoded, const std::vector<Affine>& matrices, std::uint32_t width,
                 std::uint32_t height) {
    encoded.paths.assign(static_cast<std::size_t>(encoded.path_count) * 16, 0);
    for (std::uint32_t p = 0; p < encoded.path_count; ++p) {
        const Affine m = matrices.empty() ? Affine{} : matrices[p];
        const auto& box = encoded.local_boxes[p];
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        for (const path::PointF corner :
             {path::PointF{box[0], box[1]}, path::PointF{box[2], box[1]},
              path::PointF{box[2], box[3]}, path::PointF{box[0], box[3]}}) {
            const path::PointF q = apply(m, corner);
            min_x = std::min(min_x, q.x);
            max_x = std::max(max_x, q.x);
            min_y = std::min(min_y, q.y);
            max_y = std::max(max_y, q.y);
        }
        const auto clamp_to = [](float v, std::uint32_t limit) {
            return static_cast<std::uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
        };
        std::uint32_t* record = &encoded.paths[static_cast<std::size_t>(p) * 16];
        record[0] = bits(m.a);
        record[1] = bits(m.b);
        record[2] = bits(m.c);
        record[3] = bits(m.d);
        record[4] = bits(m.e);
        record[5] = bits(m.f);
        record[6] = encoded.rules[p];
        record[7] = encoded.colors[p];
        record[8] = clamp_to(std::floor(min_x) - 1, width);
        record[9] = clamp_to(std::floor(min_y) - 1, height);
        record[10] = clamp_to(std::ceil(max_x) + 1, width);
        record[11] = clamp_to(std::ceil(max_y) + 1, height);
    }
}

[[nodiscard]] Encoded encode(const SceneDescription& scene, std::uint32_t width,
                             std::uint32_t height, float max_scale) {
    Encoded result;
    result.arc_edges = arc_edges_for(scene, max_scale);
    const std::uint32_t stride = 4 + 2 * (result.arc_edges + 2);
    for (std::size_t p = 0; p < scene.gpu.size(); ++p) {
        const GpuShape& shape = scene.gpu[p];
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        if (shape.half_width > 0.0f) {
            const float hw = shape.half_width;
            const std::size_t count = shape.contours.size() / 4;
            // Dashed: cut every segment into chunks no longer than one pattern period, so a chunk
            // holds at most (on entries + 1) dashes and every record has a fixed number of piece
            // slots. The cut does not depend on the offset, so animating the offset re-encodes
            // nothing.
            float period = 0.0f;
            for (const float v : shape.dash) {
                period += v;
            }
            const std::uint32_t pieces =
                shape.dash.empty() ? 1U : static_cast<std::uint32_t>(shape.dash.size() / 2 + 1);
            struct Chunk {
                float x0, y0, x1, y1, start;
            };
            std::vector<Chunk> chunks;
            float length_so_far = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                const float* s = &shape.contours[i * 4];
                const float length = std::hypot(s[2] - s[0], s[3] - s[1]);
                const std::size_t parts =
                    period > 0.0f ? std::max<std::size_t>(
                                        1, static_cast<std::size_t>(std::ceil(length / period)))
                                  : 1;
                for (std::size_t part = 0; part < parts; ++part) {
                    const float t0 = static_cast<float>(part) / static_cast<float>(parts);
                    const float t1 = static_cast<float>(part + 1) / static_cast<float>(parts);
                    chunks.push_back({s[0] + (s[2] - s[0]) * t0, s[1] + (s[3] - s[1]) * t0,
                                      s[0] + (s[2] - s[0]) * t1, s[1] + (s[3] - s[1]) * t1,
                                      length_so_far + length * t0});
                }
                length_so_far += length;
            }
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const Chunk& c = chunks[i];
                const bool closed = !shape.open;
                const bool has_prev = closed || i > 0;
                const Chunk& before = i > 0 ? chunks[i - 1] : chunks.back();
                const bool last = i + 1 == chunks.size();
                const std::uint32_t flags = (shape.open && last ? 1U : 0U) | (has_prev ? 2U : 0U) |
                                            (i == 0 ? 4U : 0U) | (last ? 8U : 0U);
                result.strokes.insert(result.strokes.end(),
                                      {bits(c.x0), bits(c.y0), bits(c.x1), bits(c.y1),
                                       static_cast<std::uint32_t>(p), bits(hw), flags, shape.join,
                                       bits(before.x0), bits(before.y0), bits(shape.miter_limit),
                                       shape.cap, bits(c.start), result.stroke_edges, pieces, 0});
                result.stroke_edges += pieces * stride;
                min_x = std::min({min_x, c.x0, c.x1});
                max_x = std::max({max_x, c.x0, c.x1});
                min_y = std::min({min_y, c.y0, c.y1});
                max_y = std::max({max_y, c.y0, c.y1});
                // Rows the expanded edges can cross: the two long body edges, the two short ones,
                // and two wedges each walking the rim up and down, for each dash piece.
                const float dy = std::abs(c.y1 - c.y0);
                const float rows =
                    2 * (dy + 2) + 2 * (2 * hw + 2) +
                    2 * (2 * (2 * hw + 2) + static_cast<float>(result.arc_edges) + 2);
                // A miter tip can reach hw * miter_limit past the vertex: widen the row bound.
                const float reach = shape.join == 2 ? hw * std::max(shape.miter_limit, 1.0f) : hw;
                result.row_nodes +=
                    static_cast<std::uint64_t>((rows + 4 * reach) * static_cast<float>(pieces));
                ++result.stroke_count;
            }
            // A miter tip or a square cap reaches past hw: the box must hold it.
            const float reach = shape.join == 2 ? hw * std::max(shape.miter_limit, 1.0f)
                                                : (shape.cap == 2 ? hw * 1.4143f : hw);
            min_x -= reach + 1;
            min_y -= reach + 1;
            max_x += reach + 1;
            max_y += reach + 1;
            result.local_boxes.push_back({min_x, min_y, max_x, max_y});
            result.rules.push_back(shape.rule);
            result.colors.push_back(shape.rgba);
            ++result.path_count;
            continue;
        }
        for (std::size_t i = 0; i + 3 < shape.contours.size(); i += 4) {
            const float* s = &shape.contours[i];
            result.segs.insert(result.segs.end(), {bits(s[0]), bits(s[1]), bits(s[2]), bits(s[3]),
                                                   static_cast<std::uint32_t>(p), 0, 0, 0});
            min_x = std::min({min_x, s[0], s[2]});
            max_x = std::max({max_x, s[0], s[2]});
            min_y = std::min({min_y, s[1], s[3]});
            max_y = std::max({max_y, s[1], s[3]});
            const float rows = std::ceil(std::max(s[1], s[3])) - std::floor(std::min(s[1], s[3]));
            result.row_nodes += static_cast<std::uint64_t>(std::max(rows, 1.0f));
            ++result.fill_count;
        }
        result.local_boxes.push_back({min_x, min_y, max_x, max_y});
        result.rules.push_back(shape.rule);
        result.colors.push_back(shape.rgba);
        ++result.path_count;
    }
    result.seg_count = result.fill_count + result.stroke_edges;
    result.dashes.assign(static_cast<std::size_t>(result.path_count) * 12, 0);
    result.dash_base.assign(result.path_count, 0.0f);
    for (std::size_t p = 0; p < scene.gpu.size(); ++p) {
        const std::vector<float>& pattern = scene.gpu[p].dash;
        if (pattern.empty()) {
            continue;
        }
        float period = 0.0f;
        std::uint32_t* record = &result.dashes[p * 12];
        record[0] = static_cast<std::uint32_t>(pattern.size());
        for (std::size_t i = 0; i < pattern.size() && i < 8; ++i) {
            record[4 + i] = bits(pattern[i]);
            period += pattern[i];
        }
        record[2] = bits(period);
        result.dash_base[p] = scene.gpu[p].dash_offset;
        record[1] = bits(scene.gpu[p].dash_offset);
    }
    write_paths(result, {}, width, height);
    return result;
}

struct PushBlock {
    std::uint32_t width, height, path_count, seg_count, node_capacity, nodes_base, fill_count,
        exp_base, stroke_count, arc_edges, pad0, pad1;
};

double percentile(std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(q * static_cast<double>(values.size() - 1))];
}

struct Options {
    std::uint32_t width = 3840;
    std::uint32_t height = 2160;
    int iterations = 30;
    int warmup = 5;
    bool validate = false;
    bool each = false; // also compare every dashed shape of the dash scene on its own
};

// How a scene is animated after the static frame: every path gets its own transform per frame, and
// the only per-frame upload is the path records. `similarity` (rotate + uniform scale + shift) can
// be compared with ThorVG for strokes too, because the pen scales with it; `affine` (non-uniform
// scale and shear) only for fills, since ThorVG's stroker has no non-uniform pen to compare
// against.
enum class Motion { none, similarity, affine, dash };

[[nodiscard]] Affine motion_matrix(Motion motion, std::uint32_t path_index,
                                   const std::array<float, 4>& box, int frame) {
    const float t = static_cast<float>(frame);
    const float p = static_cast<float>(path_index);
    const float theta = 0.12f * t + 0.4f * p;
    const float cs = std::cos(theta), sn = std::sin(theta);
    Affine m;
    if (motion == Motion::similarity) {
        const float s = 1.0f + 0.25f * std::sin(0.3f * t + p);
        m = {s * cs, s * sn, -s * sn, s * cs, 0.0f, 0.0f};
    } else {
        const float sx = 1.2f, sy = 0.8f, k = 0.25f; // R(theta) * [[sx, k], [0, sy]]
        m = {cs * sx, sn * sx, cs * k - sn * sy, sn * k + cs * sy, 0.0f, 0.0f};
    }
    // About the path's own centre, plus a drift, so paths move rather than only turn in place.
    const float cx = (box[0] + box[2]) * 0.5f, cy = (box[1] + box[3]) * 0.5f;
    m.e = cx + 6.0f * std::sin(0.2f * t + p) - (m.a * cx + m.c * cy);
    m.f = cy + 6.0f * std::cos(0.2f * t + p) - (m.b * cx + m.d * cy);
    return m;
}

[[nodiscard]] float motion_max_scale(Motion motion) {
    return motion == Motion::similarity || motion == Motion::affine
               ? 1.6f // the Frobenius norm of the matrices above
               : 1.0f;
}

// The ThorVG-side scene under the same transforms: points are transformed, and a stroke's width
// scales with the (similarity) transform.
[[nodiscard]] std::vector<path::PathShape>
transformed_reference(const std::vector<path::PathShape>& shapes,
                      const std::vector<Affine>& matrices) {
    std::vector<path::PathShape> result = shapes;
    for (std::size_t i = 0; i < result.size(); ++i) {
        for (path::PathSegment& segment : result[i].path.segments) {
            segment.p0 = apply(matrices[i], segment.p0);
            segment.p1 = apply(matrices[i], segment.p1);
            segment.p2 = apply(matrices[i], segment.p2);
        }
        if (result[i].paint.stroke) {
            const float det =
                std::abs(matrices[i].a * matrices[i].d - matrices[i].b * matrices[i].c);
            result[i].paint.stroke->width *= std::sqrt(det);
        }
    }
    return result;
}

void run_scene(rhi::Device& device, const char* name, const SceneDescription& scene,
               const Options& options, Motion motion = Motion::none) {
    using std::printf;
    const auto encode_begin = Clock::now();
    Encoded encoded = encode(scene, options.width, options.height, motion_max_scale(motion));
    const double encode_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - encode_begin).count();
    printf("  CPU encode (flattened segments -> packed, full re-encode): %.3f ms\n", encode_ms);

    const std::uint64_t node_capacity =
        static_cast<std::uint64_t>(static_cast<double>(encoded.row_nodes) *
                                   motion_max_scale(motion)) +
        1024;
    const std::uint32_t nodes_base = 1 + encoded.path_count * options.height;
    const std::uint32_t exp_base = static_cast<std::uint32_t>(nodes_base + node_capacity * 2);
    const std::size_t work_words = exp_base + static_cast<std::size_t>(encoded.stroke_edges) * 8;
    printf("\n[%s: %zu paths, %u fill + %u stroke segments (%u expanded edges), %llu row nodes]\n",
           name, scene.gpu.size(), encoded.fill_count, encoded.stroke_count, encoded.stroke_edges,
           static_cast<unsigned long long>(encoded.row_nodes));
    const std::size_t pixel_bytes =
        static_cast<std::size_t>(options.width) * options.height * sizeof(std::uint32_t);

    const auto seg_buffer =
        device.create_buffer({std::max<std::size_t>(encoded.segs.size(), 8) * sizeof(std::uint32_t),
                              rhi::BufferUsage::Storage});
    if (!encoded.segs.empty()) {
        seg_buffer->write(encoded.segs.data(), encoded.segs.size() * sizeof(std::uint32_t));
    }
    std::vector<std::uint32_t> stroke_words = encoded.strokes;
    if (stroke_words.empty()) {
        stroke_words.assign(16, 0); // a storage buffer cannot be empty
    }
    const auto dash_buffer = device.create_buffer(
        {encoded.dashes.size() * sizeof(std::uint32_t), rhi::BufferUsage::Storage});
    dash_buffer->write(encoded.dashes.data(), encoded.dashes.size() * sizeof(std::uint32_t));
    const auto stroke_buffer = device.create_buffer(
        {stroke_words.size() * sizeof(std::uint32_t), rhi::BufferUsage::Storage});
    stroke_buffer->write(stroke_words.data(), stroke_words.size() * sizeof(std::uint32_t));
    const auto path_buffer = device.create_buffer(
        {encoded.paths.size() * sizeof(std::uint32_t), rhi::BufferUsage::Storage});
    path_buffer->write(encoded.paths.data(), encoded.paths.size() * sizeof(std::uint32_t));
    rhi::BufferDesc rw{work_words * sizeof(std::uint32_t), rhi::BufferUsage::Storage};
    rw.gpu_read_write = true;
    const auto work_buffer = device.create_buffer(rw);
    rw.size = pixel_bytes;
    const auto out_buffer = device.create_buffer(rw);
    const auto readback = device.create_buffer({pixel_bytes, rhi::BufferUsage::Readback});
    const auto counter_readback = device.create_buffer({4, rhi::BufferUsage::Readback});
    printf("  bytes uploaded: %zu once (segments + strokes + paths), 0 per frame; GPU memory: work "
           "%.1f MiB, output %.1f MiB\n",
           (encoded.segs.size() + encoded.strokes.size() + encoded.paths.size()) *
               sizeof(std::uint32_t),
           static_cast<double>(work_words * 4) / (1024.0 * 1024.0),
           static_cast<double>(pixel_bytes) / (1024.0 * 1024.0));

    const auto make_pipeline = [&](const std::uint32_t* spirv, std::size_t size) {
        rhi::ComputePipelineDesc desc;
        desc.spirv = spirv;
        desc.spirv_size = size;
        desc.storage_buffer_count = 6;
        desc.push_constant_size = sizeof(PushBlock);
        return device.create_compute_pipeline(desc);
    };
    const auto clear = make_pipeline(kClearSpv, sizeof(kClearSpv));
    const auto expand = make_pipeline(kStrokeSpv, sizeof(kStrokeSpv));
    const auto bin = make_pipeline(kBinSpv, sizeof(kBinSpv));
    const auto fine = make_pipeline(kFineSpv, sizeof(kFineSpv));

    const PushBlock push{options.width,
                         options.height,
                         encoded.path_count,
                         encoded.seg_count,
                         static_cast<std::uint32_t>(node_capacity),
                         nodes_base,
                         encoded.fill_count,
                         exp_base,
                         encoded.stroke_count,
                         encoded.arc_edges,
                         0,
                         0};
    const auto record = [&](rhi::CommandList& list, bool with_copy) {
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
            list.bind_compute_pipeline(pipeline);
            list.bind_compute_buffer(0, *seg_buffer);
            list.bind_compute_buffer(1, *path_buffer);
            list.bind_compute_buffer(2, *work_buffer);
            list.bind_compute_buffer(3, *out_buffer);
            list.bind_compute_buffer(4, *stroke_buffer);
            list.bind_compute_buffer(5, *dash_buffer);
            list.compute_push_constants(&push, sizeof(push));
        };
        bind_all(*clear);
        list.dispatch(static_cast<std::uint32_t>((nodes_base + 255) / 256), 1, 1);
        list.compute_barrier();
        if (encoded.stroke_count > 0) {
            bind_all(*expand);
            list.dispatch((encoded.stroke_count + 255) / 256, 1, 1);
            list.compute_barrier();
        }
        bind_all(*bin);
        list.dispatch((encoded.seg_count + 255) / 256, 1, 1);
        list.compute_barrier();
        bind_all(*fine);
        list.dispatch((options.width + 15) / 16, (options.height + 15) / 16, 1);
        if (with_copy) {
            list.copy_buffer(*out_buffer, *readback, pixel_bytes);
            list.copy_buffer(*work_buffer, *counter_readback, 4);
        }
    };

    std::vector<double> gpu_ms, cpu_ms;
    for (int i = 0; i < options.warmup + options.iterations; ++i) {
        const auto list = device.create_command_list();
        const bool timed = list->begin_gpu_timestamps(0);
        const auto begin = Clock::now();
        record(*list, false);
        list->submit_async();
        const double record_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        if (!list->wait(10000)) {
            throw std::runtime_error("the compute submission did not finish");
        }
        double ms = 0.0;
        if (i >= options.warmup) {
            cpu_ms.push_back(record_ms);
            if (timed && list->gpu_frame_time_ms(ms)) {
                gpu_ms.push_back(ms);
            }
        }
    }
    if (gpu_ms.empty()) {
        printf("  GPU time: no usable timestamps on this device\n");
    } else {
        printf(
            "  GPU time per frame: min %.2f ms, p50 %.2f ms, p95 %.2f ms (%zu runs; clear + bin + "
            "fine at %ux%u)\n",
            *std::min_element(gpu_ms.begin(), gpu_ms.end()), percentile(gpu_ms, 0.5),
            percentile(gpu_ms, 0.95), gpu_ms.size(), options.width, options.height);
    }
    printf("  CPU record + submit per frame: p50 %.3f ms\n", percentile(cpu_ms, 0.5));

    const auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    const bool have_thorvg = rasterizer != nullptr && rasterizer->backend_name() == "thorvg-cpu";
    const std::size_t pixel_count = pixel_bytes / 4;

    // One frame with the readback copies, for the overflow check and the pixel comparison.
    const auto frame_and_compare = [&](const char* label,
                                       const std::vector<path::PathShape>& reference_shapes) {
        const auto list = device.create_command_list();
        record(*list, true);
        list->submit();
        std::uint32_t used = 0;
        counter_readback->read(&used, 4);
        printf("  %sbin nodes used %u of %llu%s\n", label, used,
               static_cast<unsigned long long>(node_capacity),
               used > node_capacity ? "  OVERFLOW" : "");
        if (!have_thorvg) {
            printf("  ThorVG unavailable in this build: no pixel comparison\n");
            return;
        }
        std::vector<std::uint8_t> gpu_pixels(pixel_bytes);
        readback->read(gpu_pixels.data(), pixel_bytes);
        std::vector<std::uint8_t> reference(pixel_bytes, 0);
        const path::RasterTarget target{std::span<std::uint8_t>(reference), options.width,
                                        options.height, options.width * 4U};
        const auto thor_begin = Clock::now();
        if (!rasterizer->rasterize(reference_shapes, target).has_value()) {
            printf("  ThorVG rasterize failed\n");
            return;
        }
        printf("  %sThorVG CPU raster of the same scene: %.2f ms\n", label,
               std::chrono::duration<double, std::milli>(Clock::now() - thor_begin).count());
        std::uint64_t over4 = 0, over16 = 0;
        std::uint64_t sum = 0;
        int max_diff = 0;
        for (std::size_t i = 0; i < pixel_count; ++i) {
            int worst = 0;
            // Straight RGBA is meaningless where alpha is ~0, so compare premultiplied channels.
            const int ga = static_cast<int>(gpu_pixels[i * 4 + 3]);
            const int ra = static_cast<int>(reference[i * 4 + 3]);
            for (int c = 0; c < 4; ++c) {
                const int g =
                    c == 3 ? ga : (static_cast<int>(gpu_pixels[i * 4 + c]) * ga + 127) / 255;
                const int rv =
                    c == 3 ? ra : (static_cast<int>(reference[i * 4 + c]) * ra + 127) / 255;
                const int d = std::abs(g - rv);
                worst = std::max(worst, d);
                sum += static_cast<std::uint64_t>(d);
            }
            max_diff = std::max(max_diff, worst);
            over4 += worst > 4 ? 1 : 0;
            over16 += worst > 16 ? 1 : 0;
        }
        printf("  %svs ThorVG: mean abs diff %.4f per channel, max %d, pixels off by >4: %.4f%%, "
               ">16: %.4f%%\n",
               label, static_cast<double>(sum) / static_cast<double>(pixel_count * 4), max_diff,
               100.0 * static_cast<double>(over4) / static_cast<double>(pixel_count),
               100.0 * static_cast<double>(over16) / static_cast<double>(pixel_count));
    };
    frame_and_compare("", scene.reference);
    if (motion == Motion::none) {
        return;
    }

    // Animated frames: only the path records change (CPU rewrite + upload), the segments and the
    // stroke centerlines stay where they are.
    const auto matrices_at = [&](int frame) {
        std::vector<Affine> matrices(encoded.path_count);
        for (std::uint32_t p = 0; p < encoded.path_count; ++p) {
            matrices[p] = motion_matrix(motion, p, encoded.local_boxes[p], frame);
        }
        return matrices;
    };
    // Dash motion marches every pattern along its path: the offset word of each path, 4 bytes per
    // dashed path, is all that changes.
    constexpr float kDashSpeed = 2.5f;
    const auto upload_frame = [&](int frame) {
        if (motion == Motion::dash) {
            for (std::uint32_t p = 0; p < encoded.path_count; ++p) {
                if (encoded.dashes[static_cast<std::size_t>(p) * 12] != 0) {
                    encoded.dashes[static_cast<std::size_t>(p) * 12 + 1] =
                        bits(encoded.dash_base[p] + kDashSpeed * static_cast<float>(frame));
                }
            }
            dash_buffer->write(encoded.dashes.data(),
                               encoded.dashes.size() * sizeof(std::uint32_t));
            return;
        }
        write_paths(encoded, matrices_at(frame), options.width, options.height);
        path_buffer->write(encoded.paths.data(), encoded.paths.size() * sizeof(std::uint32_t));
    };
    const auto reference_at = [&](int frame) {
        if (motion != Motion::dash) {
            return transformed_reference(scene.reference, matrices_at(frame));
        }
        std::vector<path::PathShape> shapes = scene.reference;
        for (std::size_t p = 0; p < shapes.size(); ++p) {
            if (!scene.gpu[p].dash.empty()) {
                shapes[p].path =
                    dashed_path(scene.gpu[p],
                                scene.gpu[p].dash_offset + kDashSpeed * static_cast<float>(frame));
            }
        }
        return shapes;
    };
    constexpr int kCheckFrame = 7;
    upload_frame(kCheckFrame);
    frame_and_compare("[animated, frame 7] ", reference_at(kCheckFrame));

    std::vector<double> update_ms, animated_gpu_ms;
    for (int i = 0; i < options.warmup + options.iterations; ++i) {
        const auto update_begin = Clock::now();
        upload_frame(10 + i);
        const double update =
            std::chrono::duration<double, std::milli>(Clock::now() - update_begin).count();
        const auto list = device.create_command_list();
        const bool timed = list->begin_gpu_timestamps(0);
        record(*list, false);
        list->submit_async();
        if (!list->wait(10000)) {
            throw std::runtime_error("the compute submission did not finish");
        }
        double ms = 0.0;
        if (i >= options.warmup) {
            update_ms.push_back(update);
            if (timed && list->gpu_frame_time_ms(ms)) {
                animated_gpu_ms.push_back(ms);
            }
        }
    }
    const std::size_t animated_bytes =
        (motion == Motion::dash ? encoded.dashes.size() : encoded.paths.size()) *
        sizeof(std::uint32_t);
    printf("  animated frame: CPU rewrite + upload p50 %.3f ms for %zu bytes (%u paths), versus "
           "%.3f ms "
           "for a full re-encode; GPU p50 %.2f ms (min %.2f)\n",
           percentile(update_ms, 0.5), animated_bytes, encoded.path_count, encode_ms,
           animated_gpu_ms.empty() ? 0.0 : percentile(animated_gpu_ms, 0.5),
           animated_gpu_ms.empty()
               ? 0.0
               : *std::min_element(animated_gpu_ms.begin(), animated_gpu_ms.end()));
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--each") {
            options.each = true;
        } else if (arg == "--validate") {
            options.validate = true;
        } else if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = std::atoi(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            options.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            options.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
    }
    try {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Vulkan;
        desc.validation = options.validate;
        const auto device = tessera::rendering::vulkan::create_vulkan_device(desc);
        if (!device->supports_compute()) {
            std::printf("compute_raster: this device reports no compute\n");
            return 0;
        }
        std::printf("compute_raster: %ux%u, %d iterations after %d warm-up\n", options.width,
                    options.height, options.iterations, options.warmup);
        const auto width = static_cast<float>(options.width);
        const auto height = static_cast<float>(options.height);
        g_gpu_stroke = false;
        run_scene(*device, "sparse, ring strokes (CPU contours)", sparse_scene(), options);
        run_scene(*device, "dense, ring strokes (CPU contours)", dense_scene(width, height),
                  options);
        g_gpu_stroke = true;
        run_scene(*device, "sparse, GPU strokes", sparse_scene(), options);
        run_scene(*device, "dense, GPU strokes", dense_scene(width, height), options);
        run_scene(*device, "open polylines, GPU strokes (round joins and caps)", polyline_scene(),
                  options);
        run_scene(*device, "miter and bevel joins, butt and square caps, closed polygons",
                  join_scene(), options);
        run_scene(*device, "animated: dense scene, per-path rotate + scale + drift",
                  dense_scene(width, height), options, Motion::similarity);
        run_scene(*device, "animated: sparse scene, per-path rotate + scale + drift",
                  sparse_scene(), options, Motion::similarity);
        run_scene(*device, "animated: joins and caps, per-path rotate + scale + drift",
                  join_scene(), options, Motion::similarity);
        g_fills_only = true;
        run_scene(*device, "animated: sparse fills, non-uniform scale + shear", sparse_scene(),
                  options, Motion::affine);
        g_fills_only = false;
        run_scene(*device, "dashes: patterns, caps, joins, closed polygon, circle", dash_scene(),
                  options);
        if (options.each) {
            const SceneDescription all = dash_scene();
            for (std::size_t i = 0; i < all.gpu.size(); ++i) {
                SceneDescription one;
                one.gpu.push_back(all.gpu[i]);
                one.reference.push_back(all.reference[i]);
                run_scene(*device, ("dash shape " + std::to_string(i)).c_str(), one, options);
            }
        }
        run_scene(*device, "animated: dash offset marching", dash_scene(), options, Motion::dash);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "compute_raster: %s\n", error.what());
        return 1;
    }
    return 0;
}
