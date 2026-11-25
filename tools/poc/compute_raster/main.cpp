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

#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_scale.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
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
#include <filesystem>
#include <fstream>
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

// The scene the GPU rasterizer draws: the production encoder's shape type, so the spike measures
// the code the product runs.
using GpuShape = path::GpuPathShape;

[[nodiscard]] path::GpuJoin join_code(path::LineJoin join) {
    return join == path::LineJoin::round   ? path::GpuJoin::round
           : join == path::LineJoin::bevel ? path::GpuJoin::bevel
                                           : path::GpuJoin::miter;
}

[[nodiscard]] path::GpuCap cap_code(path::LineCap cap) {
    return cap == path::LineCap::round  ? path::GpuCap::round
           : cap == path::LineCap::butt ? path::GpuCap::butt
                                        : path::GpuCap::square;
}

// How the scene builders emit a stroke: false = ring contours on the CPU, true = GPU stroker.
bool g_gpu_stroke = true;
// Drops the strokes from the sparse and dense scenes (for the affine comparison, see Motion).
bool g_fills_only = false;

using Affine = path::PathAffine;

[[nodiscard]] path::PointF apply(const Affine& m, path::PointF p) {
    return m.apply(p);
}

struct SceneDescription {
    std::vector<path::PathShape> reference;
    std::vector<GpuShape> gpu;
};

[[nodiscard]] std::uint32_t pack(path::Rgba8 c) {
    return path::pack_rgba(c);
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
    shape.dash = path::normalized_dash_pattern(style.dash);
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
    shape.dash = path::normalized_dash_pattern(std::move(dash));
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

// ---- SVG route: the decoded document (ThorVG is the parser) -> the compute rasterizer's input
// -------

// One scene from decoded document shapes (already scaled to device pixels), through the production
// flattener, plus the shapes themselves as the ThorVG reference.
[[nodiscard]] SceneDescription svg_scene(std::span<const path::PathShape> shapes) {
    SceneDescription scene;
    scene.reference.assign(shapes.begin(), shapes.end());
    scene.gpu = path::gpu_scene_from_shapes(shapes, 0.05f).shapes;
    return scene;
}

// ---- GPU data
// ------------------------------------------------------------------------------------

using Encoded = path::EncodedGpuScene;

[[nodiscard]] Encoded encode(const SceneDescription& scene, std::uint32_t width,
                             std::uint32_t height, float max_scale) {
    return path::encode_gpu_scene(path::GpuPathScene{scene.gpu}, width, height, max_scale);
}

void write_paths(Encoded& encoded, const std::vector<Affine>& matrices, std::uint32_t width,
                 std::uint32_t height) {
    path::write_path_records(encoded, matrices, width, height);
}

using PushBlock = path::PathRasterPush;

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
    bool keep_pixels = false; // RunResult carries both images (the SVG mode's --dump)
    bool each = false;        // also compare every dashed shape of the dash scene on its own
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

// What one scene run measured; the SVG mode prints these as one line per icon.
struct RunResult {
    double gpu_p50_ms = 0.0;
    double wall_p50_ms = 0.0; // record + submit + wait: what a texture-cache miss would wait
    double thorvg_ms = 0.0;
    double encode_ms = 0.0;
    int max_diff = 0;
    double over4_percent = 0.0;
    double over16_percent = 0.0;
    bool compared = false;
    std::vector<std::uint8_t> gpu_pixels, reference_pixels; // kept when Options::keep_pixels
};

bool g_quiet = false;

template<typename... Args> void say(const char* format, Args... args) {
    if (g_quiet) {
        return;
    }
    if constexpr (sizeof...(Args) == 0) {
        std::fputs(format, stdout);
    } else {
        std::printf(format, args...); // NOLINT(clang-diagnostic-format-nonliteral)
    }
}

RunResult run_scene(rhi::Device& device, const char* name, const SceneDescription& scene,
                    const Options& options, Motion motion = Motion::none) {
    RunResult result;
    const auto encode_begin = Clock::now();
    Encoded encoded = encode(scene, options.width, options.height, motion_max_scale(motion));
    const double encode_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - encode_begin).count();
    say("  CPU encode (flattened segments -> packed, full re-encode): %.3f ms\n", encode_ms);

    const auto layout =
        path::plan_path_raster(encoded, options.width, options.height, 0, motion_max_scale(motion));
    if (!layout) {
        throw std::runtime_error("the scene does not fit the rasterizer's 32-bit indices");
    }
    const std::uint32_t nodes_base = layout->push.nodes_base;
    const std::size_t work_words = layout->work_words;
    say("\n[%s: %zu paths, %u fill + %u stroke segments (%u expanded edges), %llu row nodes]\n",
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
    say("  bytes uploaded: %zu once (segments + strokes + paths), 0 per frame; GPU memory: work "
        "%.1f MiB, output %.1f MiB\n",
        (encoded.segs.size() + encoded.strokes.size() + encoded.paths.size()) *
            sizeof(std::uint32_t),
        static_cast<double>(work_words * 4) / (1024.0 * 1024.0),
        static_cast<double>(pixel_bytes) / (1024.0 * 1024.0));

    const auto make_pipeline = [&](const std::uint32_t* spirv, std::size_t size) {
        rhi::ComputePipelineDesc desc;
        desc.spirv = spirv;
        desc.spirv_size = size;
        desc.storage_buffer_count = 7;
        desc.push_constant_size = sizeof(std::uint32_t); // job_base
        return device.create_compute_pipeline(desc);
    };
    const auto clear = make_pipeline(kClearSpv, sizeof(kClearSpv));
    const auto expand = make_pipeline(kStrokeSpv, sizeof(kStrokeSpv));
    const auto bin = make_pipeline(kBinSpv, sizeof(kBinSpv));
    const auto fine = make_pipeline(kFineSpv, sizeof(kFineSpv));

    const path::PathRasterJob job_record = path::single_raster_job(*layout);
    const auto job_buffer = device.create_buffer({sizeof(job_record), rhi::BufferUsage::Storage});
    job_buffer->write(&job_record, sizeof(job_record));
    const std::uint32_t job_base = 0;
    const auto record = [&](rhi::CommandList& list, bool with_copy) {
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
            list.bind_compute_pipeline(pipeline);
            list.bind_compute_buffer(0, *seg_buffer);
            list.bind_compute_buffer(1, *path_buffer);
            list.bind_compute_buffer(2, *work_buffer);
            list.bind_compute_buffer(3, *out_buffer);
            list.bind_compute_buffer(4, *stroke_buffer);
            list.bind_compute_buffer(5, *dash_buffer);
            list.bind_compute_buffer(6, *job_buffer);
            list.compute_push_constants(&job_base, sizeof(job_base));
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

    std::vector<double> gpu_ms, cpu_ms, wall_ms;
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
        const double wall = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        double ms = 0.0;
        if (i >= options.warmup) {
            cpu_ms.push_back(record_ms);
            wall_ms.push_back(wall);
            if (timed && list->gpu_frame_time_ms(ms)) {
                gpu_ms.push_back(ms);
            }
        }
    }
    result.encode_ms = encode_ms;
    result.wall_p50_ms = percentile(wall_ms, 0.5);
    result.gpu_p50_ms = gpu_ms.empty() ? 0.0 : percentile(gpu_ms, 0.5);
    if (gpu_ms.empty()) {
        say("  GPU time: no usable timestamps on this device\n");
    } else {
        say("  GPU time per frame: min %.2f ms, p50 %.2f ms, p95 %.2f ms (%zu runs; clear + bin + "
            "fine at %ux%u)\n",
            *std::min_element(gpu_ms.begin(), gpu_ms.end()), percentile(gpu_ms, 0.5),
            percentile(gpu_ms, 0.95), gpu_ms.size(), options.width, options.height);
    }
    say("  CPU record + submit per frame: p50 %.3f ms\n", percentile(cpu_ms, 0.5));

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
        say("  %sbin nodes used %u of %llu%s\n", label, used,
            static_cast<unsigned long long>(layout->push.node_capacity),
            used > layout->push.node_capacity ? "  OVERFLOW" : "");
        if (!have_thorvg) {
            say("  ThorVG unavailable in this build: no pixel comparison\n");
            return;
        }
        std::vector<std::uint8_t> gpu_pixels(pixel_bytes);
        readback->read(gpu_pixels.data(), pixel_bytes);
        std::vector<std::uint8_t> reference(pixel_bytes, 0);
        const path::RasterTarget target{std::span<std::uint8_t>(reference), options.width,
                                        options.height, options.width * 4U};
        const auto thor_begin = Clock::now();
        if (!rasterizer->rasterize(reference_shapes, target).has_value()) {
            say("  ThorVG rasterize failed\n");
            return;
        }
        const double thor_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - thor_begin).count();
        say("  %sThorVG CPU raster of the same scene: %.2f ms\n", label, thor_ms);
        if (label[0] == 0) {
            result.thorvg_ms = thor_ms;
        }
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
        if (label[0] == 0 && options.keep_pixels) {
            result.gpu_pixels = gpu_pixels;
            result.reference_pixels = reference;
        }
        if (label[0] == 0) {
            result.compared = true;
            result.max_diff = max_diff;
            result.over4_percent =
                100.0 * static_cast<double>(over4) / static_cast<double>(pixel_count);
            result.over16_percent =
                100.0 * static_cast<double>(over16) / static_cast<double>(pixel_count);
        }
        say("  %svs ThorVG: mean abs diff %.4f per channel, max %d, pixels off by >4: %.4f%%, "
            ">16: %.4f%%\n",
            label, static_cast<double>(sum) / static_cast<double>(pixel_count * 4), max_diff,
            100.0 * static_cast<double>(over4) / static_cast<double>(pixel_count),
            100.0 * static_cast<double>(over16) / static_cast<double>(pixel_count));
    };
    frame_and_compare("", scene.reference);
    if (motion == Motion::none) {
        return result;
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
            std::vector<float> phase(encoded.path_count, kDashSpeed * static_cast<float>(frame));
            path::write_dash_offsets(encoded, phase);
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
    say("  animated frame: CPU rewrite + upload p50 %.3f ms for %zu bytes (%u paths), versus "
        "%.3f ms "
        "for a full re-encode; GPU p50 %.2f ms (min %.2f)\n",
        percentile(update_ms, 0.5), animated_bytes, encoded.path_count, encode_ms,
        animated_gpu_ms.empty() ? 0.0 : percentile(animated_gpu_ms, 0.5),
        animated_gpu_ms.empty()
            ? 0.0
            : *std::min_element(animated_gpu_ms.begin(), animated_gpu_ms.end()));
    return result;
}

// Every SVG under `directory`, decoded by the production loader (ThorVG parses; its geometry is
// what the texture cache rasterizes today) and rasterized at each size by the compute rasterizer
// and by ThorVG: one line per icon and size, then a summary per size. `wall` is record + submit +
// wait, what a texture-cache miss would wait for (the CPU path's equivalent is the ThorVG time).
void run_svg_dir(rhi::Device& device, const std::string& directory,
                 const std::vector<std::uint32_t>& sizes, const Options& options,
                 const std::string& dump_prefix) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".svg") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    struct Totals {
        int count = 0;
        double gpu = 0, wall = 0, encode = 0, thorvg = 0, over4 = 0, over16 = 0;
        int worst = 0;
        std::string worst_name;
    };
    std::vector<Totals> totals(sizes.size());
    std::printf("svg dir %s: %zu files\n", directory.c_str(), files.size());
    for (const fs::path& file : files) {
        std::ifstream stream(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
        const auto document = path::load_svg(std::span<const char>(bytes.data(), bytes.size()));
        if (!document.has_value()) {
            std::printf("  skip %s: loader error %d\n", file.filename().string().c_str(),
                        static_cast<int>(document.error()));
            continue;
        }
        const std::uint32_t longest = std::max(document->width, document->height);
        for (std::size_t s = 0; s < sizes.size(); ++s) {
            const float scale = static_cast<float>(sizes[s]) / static_cast<float>(longest);
            const std::vector<path::PathShape> scaled =
                path::scale_path_shapes(document->shapes, scale);
            const std::span<const path::PathShape> shapes =
                scaled.empty() ? std::span<const path::PathShape>(document->shapes)
                               : std::span<const path::PathShape>(scaled);
            Options sized = options;
            sized.width =
                static_cast<std::uint32_t>(std::ceil(static_cast<float>(document->width) * scale));
            sized.height =
                static_cast<std::uint32_t>(std::ceil(static_cast<float>(document->height) * scale));
            if (sized.width == 0 || sized.height == 0) {
                continue;
            }
            const SceneDescription scene = svg_scene(shapes);
            g_quiet = true;
            sized.keep_pixels = !dump_prefix.empty();
            const RunResult r = run_scene(device, "", scene, sized);
            if (!dump_prefix.empty() && !r.gpu_pixels.empty()) {
                const std::string base =
                    dump_prefix + file.stem().string() + "_" + std::to_string(sizes[s]);
                const auto raw = [&](const char* suffix, const std::vector<std::uint8_t>& px) {
                    std::ofstream out(base + suffix, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(px.data()),
                              static_cast<std::streamsize>(px.size()));
                };
                raw("_gpu.rgba", r.gpu_pixels);
                raw("_ref.rgba", r.reference_pixels);
                std::ofstream(base + ".size") << sized.width << " " << sized.height;
            }
            g_quiet = false;
            std::printf("  %-28s %5ux%-5u %3zu shapes  GPU %.3f ms  wall %.3f ms  ThorVG %.3f ms  "
                        "max %3d  >4 %.3f%%  >16 %.3f%%\n",
                        file.filename().string().c_str(), sized.width, sized.height, shapes.size(),
                        r.gpu_p50_ms, r.wall_p50_ms, r.thorvg_ms, r.max_diff, r.over4_percent,
                        r.over16_percent);
            Totals& t = totals[s];
            ++t.count;
            t.gpu += r.gpu_p50_ms;
            t.wall += r.wall_p50_ms;
            t.encode += r.encode_ms;
            t.thorvg += r.thorvg_ms;
            t.over4 += r.over4_percent;
            t.over16 += r.over16_percent;
            if (r.max_diff > t.worst) {
                t.worst = r.max_diff;
                t.worst_name = file.filename().string();
            }
        }
    }
    for (std::size_t s = 0; s < sizes.size(); ++s) {
        const Totals& t = totals[s];
        if (t.count == 0) {
            continue;
        }
        const double n = t.count;
        std::printf("SUMMARY %4u px: %d icons, mean GPU %.3f ms, wall %.3f ms, CPU encode %.3f ms "
                    "(GPU path total %.3f ms), ThorVG %.3f ms, mean >4 %.3f%%, mean >16 %.3f%%, "
                    "worst max diff %d (%s)\n",
                    sizes[s], t.count, t.gpu / n, t.wall / n, t.encode / n, (t.wall + t.encode) / n,
                    t.thorvg / n, t.over4 / n, t.over16 / n, t.worst, t.worst_name.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string svg_dir, dump_prefix;
    std::vector<std::uint32_t> svg_sizes = {32, 64, 256, 1024};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump" && i + 1 < argc) {
            dump_prefix = argv[++i];
        } else if (arg == "--svg-dir" && i + 1 < argc) {
            svg_dir = argv[++i];
        } else if (arg == "--svg-sizes" && i + 1 < argc) {
            svg_sizes.clear();
            const std::string list = argv[++i];
            for (std::size_t at = 0; at < list.size();) {
                const std::size_t comma = list.find(',', at);
                svg_sizes.push_back(
                    static_cast<std::uint32_t>(std::atoi(list.substr(at, comma - at).c_str())));
                at = comma == std::string::npos ? list.size() : comma + 1;
            }
        } else if (arg == "--each") {
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
        if (!svg_dir.empty()) {
            if (!path::svg_backend_available()) {
                std::printf(
                    "compute_raster: this build has no SVG backend (TESSERA_ENABLE_THORVG)\n");
                return 0;
            }
            run_svg_dir(*device, svg_dir, svg_sizes, options, dump_prefix);
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
