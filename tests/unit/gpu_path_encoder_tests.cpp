// SPDX-License-Identifier: MIT
//
// The GPU path rasterizer's CPU half (ADR-071): decoded shapes -> flattened scene, the scene -> the
// shader's word buffers (fill edges, stroke records, dash records, per-path records), and the work
// buffer / push block plan. No GPU is involved; tests/gpu/batched2d/path_raster_gpu_tests.cpp runs
// the same encoding through the compute passes.

#include "tessera/rendering/path/gpu_path_encoder.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace path = tessera::rendering::path;

bool g_ok = true;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "gpu_path_encoder: " << message << "\n";
        g_ok = false;
    }
}

float as_float(std::uint32_t word) {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

path::Path rect(float x0, float y0, float x1, float y1) {
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {x0, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x0, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

path::GpuPathShape line_stroke(std::vector<float> contours, float half_width) {
    path::GpuPathShape shape;
    shape.contours = std::move(contours);
    shape.half_width = half_width;
    shape.open = true;
    shape.rgba = 0xFF0000FFU;
    return shape;
}

void test_normalized_dash() {
    expect(path::normalized_dash_pattern({4, 2}) == std::vector<float>({4, 2}),
           "an even pattern stays");
    expect(path::normalized_dash_pattern({3}) == std::vector<float>({3, 3}),
           "an odd pattern repeats");
    expect(path::normalized_dash_pattern({1, 2, 3}) == std::vector<float>({1, 2, 3, 1, 2, 3}),
           "an odd pattern repeats whole");
    expect(path::normalized_dash_pattern({1, 1, 1, 1, 1, 1, 1, 1, 1, 1}).size() == 8,
           "a long pattern is cut to eight entries");
    expect(path::normalized_dash_pattern({0, 0}).empty(), "an all-zero pattern is solid");
    expect(path::normalized_dash_pattern({4, -1}).empty(), "a negative length is solid");
    expect(path::normalized_dash_pattern({}).empty(), "no pattern is solid");
}

void test_flatten() {
    path::PathShape both;
    both.path = rect(10, 10, 50, 30);
    both.paint.fill = path::Rgba8{255, 0, 0, 255};
    path::StrokeStyle stroke;
    stroke.width = 4.0f;
    stroke.color = {0, 0, 255, 255};
    stroke.join = path::LineJoin::bevel;
    stroke.cap = path::LineCap::square;
    both.paint.stroke = stroke;
    const std::array<path::PathShape, 1> shapes{both};
    const path::GpuPathScene scene = path::gpu_scene_from_shapes(shapes);
    expect(scene.shapes.size() == 2, "a filled and stroked shape becomes a fill then a stroke");
    if (scene.shapes.size() != 2) {
        return;
    }
    const auto& fill = scene.shapes[0];
    const auto& line = scene.shapes[1];
    expect(fill.half_width == 0.0f && fill.contours.size() == 16, "the fill closes its rectangle");
    expect(fill.rgba == 0xFF0000FFU, "the fill color packs R in the low byte");
    expect(line.half_width == 2.0f && line.contour_sizes == std::vector<std::uint32_t>({4}) &&
               line.contour_open == std::vector<bool>({false}),
           "a closed stroke is one closed contour of four segments");
    expect(line.join == path::GpuJoin::bevel && line.cap == path::GpuCap::square,
           "join and cap codes map");

    path::PathShape zero;
    zero.path = rect(0, 0, 4, 4);
    zero.paint.stroke = path::StrokeStyle{{0, 0, 0, 255}, 0.0f};
    const std::array<path::PathShape, 1> zero_shapes{zero};
    expect(path::gpu_scene_from_shapes(zero_shapes).shapes.empty(),
           "a zero-width stroke draws nothing (it would read as a fill)");

    path::PathShape curve;
    curve.path.segments.push_back({path::SegmentKind::move_to, {0, 0}, {}, {}});
    curve.path.segments.push_back({path::SegmentKind::cubic_to, {0, 40}, {40, 40}, {40, 0}});
    curve.paint.fill = path::Rgba8{1, 2, 3, 255};
    const std::array<path::PathShape, 1> curves{curve};
    const auto flat = path::gpu_scene_from_shapes(curves, 0.05f);
    expect(flat.shapes.size() == 1 && flat.shapes[0].contours.size() / 4 > 8,
           "a cubic flattens to many segments");
    // Every flattened point of the curve stays on the curve (the chord error is tiny): the midpoint
    // of the arch peaks at y = 30.
    float peak = 0.0f;
    for (std::size_t i = 1; i < flat.shapes[0].contours.size(); i += 2) {
        peak = std::fmax(peak, flat.shapes[0].contours[i]);
    }
    expect(std::fabs(peak - 30.0f) < 0.1f, "the flattened arch reaches the curve's peak");
}

void test_encode_fill_and_records() {
    path::GpuPathScene scene;
    path::GpuPathShape fill;
    fill.contours = {10, 10, 50, 10, 50, 10, 50, 30, 50, 30, 10, 30, 10, 30, 10, 10};
    fill.rgba = 0x11223344U;
    fill.rule = path::GpuFillRule::even_odd;
    scene.shapes.push_back(fill);
    path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, 100, 40);
    expect(encoded.fill_count == 4 && encoded.segs.size() == 4 * path::kGpuSegWords &&
               encoded.stroke_count == 0 && encoded.path_count == 1,
           "four fill edges, one path");
    expect(encoded.paths.size() == path::kGpuPathWords, "one path record");
    const auto* record = encoded.paths.data();
    expect(as_float(record[0]) == 1.0f && as_float(record[3]) == 1.0f && record[12] == 1U,
           "the default transform is the identity, flagged");
    expect(record[6] == 1U && record[7] == 0x11223344U, "rule and color are in the record");
    expect(record[8] == 9U && record[9] == 9U && record[10] == 51U && record[11] == 31U,
           "the device box is the shape box grown by one pixel");

    const std::array<path::PathAffine, 1> shift{path::PathAffine{1, 0, 0, 1, 70, 5}};
    path::write_path_records(encoded, shift, 100, 40);
    expect(as_float(encoded.paths[4]) == 70.0f && encoded.paths[12] == 0U,
           "a translation is stored and clears the identity flag");
    expect(encoded.paths[8] == 79U && encoded.paths[10] == 100U && encoded.paths[11] == 36U,
           "the translated box is clamped to the target");
    expect(encoded.fill_count == 4 && encoded.segs.size() == 4 * path::kGpuSegWords,
           "a transform rewrite re-encodes nothing");
}

void test_encode_strokes() {
    path::GpuPathScene scene;
    scene.shapes.push_back(line_stroke({0, 0, 40, 0, 40, 0, 40, 30}, 2.0f));
    path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, 100, 100);
    expect(encoded.stroke_count == 2 && encoded.strokes.size() == 2 * path::kGpuStrokeWords &&
               encoded.fill_count == 0,
           "two stroke records for two segments");
    const std::uint32_t first_flags = encoded.strokes[6];
    const std::uint32_t second_flags = encoded.strokes[path::kGpuStrokeWords + 6];
    expect((first_flags & 4U) != 0 && (first_flags & 2U) == 0 && (first_flags & 8U) == 0,
           "the first record starts the contour with no previous vertex");
    expect((second_flags & 1U) != 0 && (second_flags & 2U) != 0 && (second_flags & 8U) != 0,
           "the last record has a previous vertex, ends the contour and caps an open end");
    expect(as_float(encoded.strokes[5]) == 2.0f, "the half width is in the record");
    expect(encoded.seg_count == encoded.stroke_edges && encoded.stroke_edges > 0,
           "stroke edges are the bin pass's segments");
    // The second record's first slot follows the first record's slots.
    expect(encoded.strokes[path::kGpuStrokeWords + 13] ==
               encoded.strokes[13] + encoded.strokes[14] * (7 + 2 * (encoded.arc_edges + 2)),
           "edge slots are laid out back to back");
}

void test_encode_dashes() {
    path::GpuPathScene scene;
    path::GpuPathShape dashed = line_stroke({0, 0, 100, 0}, 1.0f);
    dashed.dash = {10, 5};
    dashed.dash_offset = 3.0f;
    scene.shapes.push_back(dashed);
    scene.shapes.push_back(line_stroke({0, 20, 100, 20}, 1.0f)); // solid
    path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, 200, 50);
    // 100 / 15 -> 7 chunks no longer than one period; the solid line is one record.
    expect(encoded.stroke_count == 8, "a dashed segment is cut into period-long chunks");
    expect(encoded.dashes.size() == 2 * path::kGpuDashWords && encoded.dashes[0] == 2U &&
               as_float(encoded.dashes[1]) == 3.0f && as_float(encoded.dashes[2]) == 15.0f &&
               as_float(encoded.dashes[4]) == 10.0f && as_float(encoded.dashes[5]) == 5.0f,
           "the dash record holds count, offset, period and lengths");
    expect(encoded.dashes[path::kGpuDashWords] == 0U, "a solid path has no dash entries");
    const std::array<float, 2> phase{4.0f, 9.0f};
    path::write_dash_offsets(encoded, phase);
    expect(as_float(encoded.dashes[1]) == 7.0f, "a dash phase adds to the encoded offset");
    expect(encoded.dashes[path::kGpuDashWords] == 0U &&
               as_float(encoded.dashes[path::kGpuDashWords + 1]) == 0.0f,
           "a solid path ignores the phase");
}

void test_plan() {
    path::GpuPathScene scene;
    scene.shapes.push_back(line_stroke({0, 0, 40, 0}, 2.0f));
    path::GpuPathShape fill;
    fill.contours = {0, 0, 10, 0, 10, 0, 10, 10, 10, 10, 0, 0};
    scene.shapes.push_back(fill);
    const path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, 100, 40);
    const auto tight = path::plan_path_raster(encoded, 100, 40);
    expect(tight && tight->push.out_stride == 100 && tight->output_words == 4000,
           "stride 0 means tightly packed");
    const auto padded = path::plan_path_raster(encoded, 100, 40, 128);
    expect(padded && padded->push.out_stride == 128 && padded->output_words == 128U * 40U,
           "a copy-aligned stride is kept");
    expect(!path::plan_path_raster(encoded, 100, 40, 99), "a stride shorter than a row is refused");
    expect(!path::plan_path_raster(encoded, 0, 40), "an empty target is refused");
    if (tight) {
        const auto& push = tight->push;
        expect(push.nodes_base == 1 + 2 * 40 && push.fill_count == encoded.fill_count &&
                   push.stroke_count == encoded.stroke_count &&
                   push.seg_count == encoded.seg_count && push.arc_edges == encoded.arc_edges &&
                   push.path_count == 2,
               "the push block mirrors the encoding");
        expect(push.exp_base == push.nodes_base + push.node_capacity * 2 &&
                   tight->work_words ==
                       push.exp_base + std::size_t{encoded.stroke_edges} * path::kGpuSegWords,
               "the work buffer holds counter, heads, nodes and expanded stroke edges");
    }
    const auto scaled = path::plan_path_raster(encoded, 100, 40, 0, 3.0f);
    expect(scaled && tight && scaled->push.node_capacity > tight->push.node_capacity,
           "a larger scale bound reserves more bin nodes");
}

} // namespace

int main() {
    test_normalized_dash();
    test_flatten();
    test_encode_fill_and_records();
    test_encode_strokes();
    test_encode_dashes();
    test_plan();
    if (!g_ok) {
        return 1;
    }
    std::cout << "gpu_path_encoder: ok\n";
    return 0;
}
