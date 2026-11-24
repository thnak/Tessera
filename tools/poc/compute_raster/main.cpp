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

[[nodiscard]] path::PathShape stroked(path::Path geometry, path::Rgba8 color, float width) {
    path::PathShape shape;
    shape.path = std::move(geometry);
    path::StrokeStyle stroke;
    stroke.color = color;
    stroke.width = width;
    stroke.cap = path::LineCap::round;
    stroke.join = path::LineJoin::round;
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
};

// How the scene builders emit a stroke: false = ring contours on the CPU, true = GPU stroker.
bool g_gpu_stroke = true;

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

// An open polyline with round joins and caps: the shape a ring cannot express.
struct Pt {
    float x, y;
};

void add_open_polyline(SceneDescription& scene, const std::vector<Pt>& points, path::Rgba8 color,
                       float width) {
    path::Path geometry;
    geometry.segments.push_back({path::SegmentKind::move_to, {points[0].x, points[0].y}, {}, {}});
    for (std::size_t i = 1; i < points.size(); ++i) {
        geometry.segments.push_back(
            {path::SegmentKind::line_to, {points[i].x, points[i].y}, {}, {}});
    }
    scene.reference.push_back(stroked(std::move(geometry), color, width));
    GpuShape shape;
    shape.rgba = pack(color);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        shape.contours.insert(shape.contours.end(),
                              {points[i].x, points[i].y, points[i + 1].x, points[i + 1].y});
    }
    shape.half_width = width * 0.5f;
    shape.open = true;
    scene.gpu.push_back(std::move(shape));
}

[[nodiscard]] SceneDescription polyline_scene() {
    SceneDescription scene;
    // Sharp turns of every kind (acute, right angle, obtuse, a reversal), thick and thin.
    add_open_polyline(scene, {{300, 300}, {900, 380}, {500, 700}, {1200, 760}, {1250, 400}},
                      {40, 160, 220, 255}, 24.0f);
    add_open_polyline(scene, {{1700, 300}, {2400, 300}, {2400, 900}, {1800, 900}, {1800, 320}},
                      {220, 80, 60, 230}, 40.0f);
    add_open_polyline(scene, {{3000, 300}, {3600, 700}, {3000, 1100}, {3500, 1500}, {3100, 1900}},
                      {240, 200, 60, 255}, 9.0f);
    add_open_polyline(scene, {{600, 1300}, {1500, 1300}, {700, 1320}}, {120, 220, 120, 255}, 18.0f);
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
    std::vector<std::uint32_t> strokes; // 12 words per stroke segment
    std::vector<std::uint32_t> paths;
    std::uint32_t seg_count = 0; // fill edges + GPU-expanded stroke edges (bin threads)
    std::uint32_t fill_count = 0;
    std::uint32_t stroke_count = 0;
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
[[nodiscard]] std::uint32_t arc_edges_for(const SceneDescription& scene) {
    float widest = 0.0f;
    for (const GpuShape& shape : scene.gpu) {
        widest = std::max(widest, shape.half_width);
    }
    if (widest <= 0.0f) {
        return 0;
    }
    const float step = std::acos(std::max(0.0f, 1.0f - 0.05f / widest)); // max angle per edge
    const auto edges = static_cast<std::uint32_t>(std::ceil(3.14159265f / std::max(step, 0.01f)));
    return std::clamp<std::uint32_t>(edges, 8, 64);
}

[[nodiscard]] Encoded encode(const SceneDescription& scene, std::uint32_t width,
                             std::uint32_t height) {
    Encoded result;
    result.arc_edges = arc_edges_for(scene);
    const std::uint32_t stride = 4 + 2 * (result.arc_edges + 2);
    for (std::size_t p = 0; p < scene.gpu.size(); ++p) {
        const GpuShape& shape = scene.gpu[p];
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        if (shape.half_width > 0.0f) {
            const float hw = shape.half_width;
            const std::size_t count = shape.contours.size() / 4;
            for (std::size_t i = 0; i < count; ++i) {
                const float* s = &shape.contours[i * 4];
                const bool closed = !shape.open;
                const bool has_prev = closed || i > 0;
                const float* before =
                    i > 0 ? &shape.contours[(i - 1) * 4] : &shape.contours[(count - 1) * 4];
                const std::uint32_t flags =
                    (shape.open && i + 1 == count ? 1U : 0U) | (has_prev ? 2U : 0U);
                result.strokes.insert(result.strokes.end(),
                                      {bits(s[0]), bits(s[1]), bits(s[2]), bits(s[3]),
                                       static_cast<std::uint32_t>(p), bits(hw), flags, 0,
                                       bits(before[0]), bits(before[1]), 0, 0});
                min_x = std::min({min_x, s[0], s[2]});
                max_x = std::max({max_x, s[0], s[2]});
                min_y = std::min({min_y, s[1], s[3]});
                max_y = std::max({max_y, s[1], s[3]});
                // Rows the expanded edges can cross: the two long body edges, the two short ones,
                // and two wedges each walking the rim up and down.
                const float dy = std::abs(s[3] - s[1]);
                const float rows =
                    2 * (dy + 2) + 2 * (2 * hw + 2) +
                    2 * (2 * (2 * hw + 2) + static_cast<float>(result.arc_edges) + 2);
                result.row_nodes += static_cast<std::uint64_t>(rows);
                ++result.stroke_count;
            }
            min_x -= hw + 1;
            min_y -= hw + 1;
            max_x += hw + 1;
            max_y += hw + 1;
            const auto clamp_to = [](float v, std::uint32_t limit) {
                return static_cast<std::uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
            };
            result.paths.insert(result.paths.end(),
                                {0, 0, clamp_to(std::floor(min_x), width),
                                 clamp_to(std::floor(min_y), height),
                                 clamp_to(std::ceil(max_x), width),
                                 clamp_to(std::ceil(max_y), height), shape.rule, shape.rgba});
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
        const auto clamp_to = [](float v, std::uint32_t limit) {
            return static_cast<std::uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
        };
        result.paths.insert(result.paths.end(),
                            {0, 0, clamp_to(std::floor(min_x), width),
                             clamp_to(std::floor(min_y), height), clamp_to(std::ceil(max_x), width),
                             clamp_to(std::ceil(max_y), height), shape.rule, shape.rgba});
        ++result.path_count;
    }
    result.seg_count = result.fill_count + result.stroke_count * stride;
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
};

void run_scene(rhi::Device& device, const char* name, const SceneDescription& scene,
               const Options& options) {
    using std::printf;
    const auto encode_begin = Clock::now();
    const Encoded encoded = encode(scene, options.width, options.height);
    const double encode_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - encode_begin).count();
    printf("  CPU encode (flattened segments -> packed, full re-encode): %.3f ms\n", encode_ms);

    const std::uint64_t node_capacity = encoded.row_nodes + 1024;
    const std::uint32_t nodes_base = 1 + encoded.path_count * options.height;
    const std::uint32_t stroke_stride = 4 + 2 * (encoded.arc_edges + 2);
    const std::uint32_t exp_base = static_cast<std::uint32_t>(nodes_base + node_capacity * 2);
    const std::size_t work_words =
        exp_base + static_cast<std::size_t>(encoded.stroke_count) * stroke_stride * 8;
    printf("\n[%s: %zu paths, %u fill + %u stroke segments (%u expanded edges), %llu row nodes]\n",
           name, scene.gpu.size(), encoded.fill_count, encoded.stroke_count,
           encoded.stroke_count * stroke_stride,
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
        stroke_words.assign(12, 0); // a storage buffer cannot be empty
    }
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
        desc.storage_buffer_count = 5;
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

    // One more frame with the readback copies, for the overflow check and the pixel comparison.
    const auto list = device.create_command_list();
    record(*list, true);
    list->submit();
    std::uint32_t used = 0;
    counter_readback->read(&used, 4);
    printf("  bin nodes used %u of %llu%s\n", used, static_cast<unsigned long long>(node_capacity),
           used > node_capacity ? "  OVERFLOW" : "");
    std::vector<std::uint8_t> gpu_pixels(pixel_bytes);
    readback->read(gpu_pixels.data(), pixel_bytes);

    const auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (rasterizer == nullptr || rasterizer->backend_name() != "thorvg-cpu") {
        printf("  ThorVG unavailable in this build: no pixel comparison\n");
        return;
    }
    std::vector<std::uint8_t> reference(pixel_bytes, 0);
    const path::RasterTarget target{std::span<std::uint8_t>(reference), options.width,
                                    options.height, options.width * 4U};
    const auto thor_begin = Clock::now();
    if (!rasterizer->rasterize(scene.reference, target).has_value()) {
        printf("  ThorVG rasterize failed\n");
        return;
    }
    printf("  ThorVG CPU raster of the same scene: %.2f ms\n",
           std::chrono::duration<double, std::milli>(Clock::now() - thor_begin).count());
    std::uint64_t over4 = 0, over16 = 0;
    std::uint64_t sum = 0;
    int max_diff = 0;
    const std::size_t pixel_count = pixel_bytes / 4;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        int worst = 0;
        // Straight RGBA is meaningless where alpha is ~0, so compare premultiplied channels.
        const int ga = static_cast<int>(gpu_pixels[i * 4 + 3]);
        const int ra = static_cast<int>(reference[i * 4 + 3]);
        for (int c = 0; c < 4; ++c) {
            const int g = c == 3 ? ga : (static_cast<int>(gpu_pixels[i * 4 + c]) * ga + 127) / 255;
            const int rv = c == 3 ? ra : (static_cast<int>(reference[i * 4 + c]) * ra + 127) / 255;
            const int d = std::abs(g - rv);
            worst = std::max(worst, d);
            sum += static_cast<std::uint64_t>(d);
        }
        max_diff = std::max(max_diff, worst);
        over4 += worst > 4 ? 1 : 0;
        over16 += worst > 16 ? 1 : 0;
    }
    printf("  vs ThorVG: mean abs diff %.4f per channel, max %d, pixels off by >4: %.4f%%, >16: "
           "%.4f%%\n",
           static_cast<double>(sum) / static_cast<double>(pixel_count * 4), max_diff,
           100.0 * static_cast<double>(over4) / static_cast<double>(pixel_count),
           100.0 * static_cast<double>(over16) / static_cast<double>(pixel_count));
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validate") {
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
    } catch (const std::exception& error) {
        std::fprintf(stderr, "compute_raster: %s\n", error.what());
        return 1;
    }
    return 0;
}
