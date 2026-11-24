// SPDX-License-Identifier: MIT
//
// GPU path rasterizer scene encoder (ADR-071): packs flattened shapes into the word buffers of
// shaders/path_raster.comp and sizes its work buffer.

#include "tessera/rendering/path/gpu_path_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace tessera::rendering::path {
namespace {

[[nodiscard]] std::uint32_t bits(float value) {
    std::uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

// Arc edges per quarter-turn-ish rim: enough that the inscribed polygon stays within ~0.05 px of
// the circle at the widest stroke of the scene.
[[nodiscard]] std::uint32_t arc_edges_for(const GpuPathScene& scene, float max_scale) {
    float widest = 0.0f;
    for (const GpuPathShape& shape : scene.shapes) {
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

} // namespace

// Rewrites the per-path records for one frame: the affine, and the device-space box that bounds the
// transformed local box (the fine pass skips a pixel outside it). An empty `matrices` is identity.
void write_path_records(EncodedGpuScene& encoded, std::span<const PathAffine> matrices,
                        std::uint32_t width, std::uint32_t height) {
    encoded.paths.assign(static_cast<std::size_t>(encoded.path_count) * kGpuPathWords, 0);
    for (std::uint32_t p = 0; p < encoded.path_count; ++p) {
        const PathAffine m = matrices.empty() ? PathAffine{} : matrices[p];
        const auto& box = encoded.local_boxes[p];
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        for (const PointF corner : {PointF{box[0], box[1]}, PointF{box[2], box[1]},
                                    PointF{box[2], box[3]}, PointF{box[0], box[3]}}) {
            const PointF q = m.apply(corner);
            min_x = std::min(min_x, q.x);
            max_x = std::max(max_x, q.x);
            min_y = std::min(min_y, q.y);
            max_y = std::max(max_y, q.y);
        }
        const auto clamp_to = [](float v, std::uint32_t limit) {
            return static_cast<std::uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
        };
        std::uint32_t* record = &encoded.paths[static_cast<std::size_t>(p) * kGpuPathWords];
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
        record[12] =
            (m.a == 1.0f && m.b == 0.0f && m.c == 0.0f && m.d == 1.0f && m.e == 0.0f && m.f == 0.0f)
                ? 1U
                : 0U;
    }
}

EncodedGpuScene encode_gpu_scene(const GpuPathScene& scene, std::uint32_t width,
                                 std::uint32_t height, float max_scale) {
    EncodedGpuScene result;
    result.arc_edges = arc_edges_for(scene, max_scale);
    const std::uint32_t stride = 7 + 2 * (result.arc_edges + 2);
    for (std::size_t p = 0; p < scene.shapes.size(); ++p) {
        const GpuPathShape& shape = scene.shapes[p];
        float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
        if (shape.half_width > 0.0f) {
            const float hw = shape.half_width;
            const std::vector<float> dash = normalized_dash_pattern(shape.dash);
            const std::size_t total = shape.contours.size() / 4;
            struct Range {
                std::size_t first, count;
                bool open;
            };
            std::vector<Range> ranges;
            if (shape.contour_sizes.empty()) {
                ranges.push_back({0, total, shape.open});
            } else {
                std::size_t at = 0;
                for (std::size_t k = 0; k < shape.contour_sizes.size(); ++k) {
                    ranges.push_back({at, shape.contour_sizes[k], shape.contour_open[k]});
                    at += shape.contour_sizes[k];
                }
            }
            // Dashed: cut every segment into chunks no longer than one pattern period, so a chunk
            // holds at most (on entries + 1) dashes and every record has a fixed number of piece
            // slots. The cut does not depend on the offset, so animating the offset re-encodes
            // nothing.
            float period = 0.0f;
            for (const float v : dash) {
                period += v;
            }
            const std::uint32_t pieces =
                dash.empty() ? 1U : static_cast<std::uint32_t>(dash.size() / 2 + 1);
            struct Chunk {
                float x0, y0, x1, y1, start;
            };
            for (const Range& range : ranges) {
                if (range.count == 0) {
                    continue;
                }
                std::vector<Chunk> chunks;
                float length_so_far = 0.0f;
                for (std::size_t i = range.first; i < range.first + range.count; ++i) {
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
                    const bool closed = !range.open;
                    const bool has_prev = closed || i > 0;
                    const Chunk& before = i > 0 ? chunks[i - 1] : chunks.back();
                    const bool last = i + 1 == chunks.size();
                    const std::uint32_t flags = (range.open && last ? 1U : 0U) |
                                                (has_prev ? 2U : 0U) | (i == 0 ? 4U : 0U) |
                                                (last ? 8U : 0U);
                    result.strokes.insert(result.strokes.end(),
                                          {bits(c.x0), bits(c.y0), bits(c.x1), bits(c.y1),
                                           static_cast<std::uint32_t>(p), bits(hw), flags,
                                           static_cast<std::uint32_t>(shape.join), bits(before.x0),
                                           bits(before.y0), bits(shape.miter_limit),
                                           static_cast<std::uint32_t>(shape.cap), bits(c.start),
                                           result.stroke_edges, pieces, 0});
                    result.stroke_edges += pieces * stride;
                    min_x = std::min({min_x, c.x0, c.x1});
                    max_x = std::max({max_x, c.x0, c.x1});
                    min_y = std::min({min_y, c.y0, c.y1});
                    max_y = std::max({max_y, c.y0, c.y1});
                    // Rows the expanded edges can cross: the two long body edges, the two short
                    // ones, and two wedges each walking the rim up and down, for each dash piece.
                    const float dy = std::abs(c.y1 - c.y0);
                    const float rows =
                        2 * (dy + 2) + 2 * (2 * hw + 2) +
                        2 * (2 * (2 * hw + 2) + static_cast<float>(result.arc_edges) + 2);
                    // A miter tip can reach hw * miter_limit past the vertex: widen the row bound.
                    const float reach =
                        shape.join == GpuJoin::miter ? hw * std::max(shape.miter_limit, 1.0f) : hw;
                    result.row_nodes +=
                        static_cast<std::uint64_t>((rows + 4 * reach) * static_cast<float>(pieces));
                    ++result.stroke_count;
                }
            }
            // A miter tip or a square cap reaches past hw: the box must hold it.
            const float reach = shape.join == GpuJoin::miter
                                    ? hw * std::max(shape.miter_limit, 1.0f)
                                    : (shape.cap == GpuCap::square ? hw * 1.4143f : hw);
            min_x -= reach + 1;
            min_y -= reach + 1;
            max_x += reach + 1;
            max_y += reach + 1;
            result.local_boxes.push_back({min_x, min_y, max_x, max_y});
            result.rules.push_back(static_cast<std::uint32_t>(shape.rule));
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
        result.rules.push_back(static_cast<std::uint32_t>(shape.rule));
        result.colors.push_back(shape.rgba);
        ++result.path_count;
    }
    result.seg_count = result.fill_count + result.stroke_edges;
    result.dashes.assign(static_cast<std::size_t>(result.path_count) * kGpuDashWords, 0);
    result.dash_base.assign(result.path_count, 0.0f);
    for (std::size_t p = 0; p < scene.shapes.size(); ++p) {
        const std::vector<float> pattern = normalized_dash_pattern(scene.shapes[p].dash);
        if (pattern.empty()) {
            continue;
        }
        float period = 0.0f;
        std::uint32_t* record = &result.dashes[p * kGpuDashWords];
        record[0] = static_cast<std::uint32_t>(pattern.size());
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            record[4 + i] = bits(pattern[i]);
            period += pattern[i];
        }
        record[2] = bits(period);
        result.dash_base[p] = scene.shapes[p].dash_offset;
        record[1] = bits(scene.shapes[p].dash_offset);
    }
    write_path_records(result, {}, width, height);
    return result;
}

void write_dash_offsets(EncodedGpuScene& encoded, std::span<const float> phase) {
    for (std::size_t p = 0; p < encoded.path_count && p < phase.size(); ++p) {
        std::uint32_t* record = &encoded.dashes[p * kGpuDashWords];
        if (record[0] != 0) {
            record[1] = bits(encoded.dash_base[p] + phase[p]);
        }
    }
}

std::optional<PathRasterLayout> plan_path_raster(const EncodedGpuScene& encoded,
                                                 std::uint32_t width, std::uint32_t height,
                                                 std::uint32_t out_stride_words, float max_scale) {
    const std::uint32_t stride = out_stride_words != 0 ? out_stride_words : width;
    if (width == 0 || height == 0 || stride < width) {
        return std::nullopt;
    }
    constexpr std::uint64_t kIndexLimit = std::numeric_limits<std::uint32_t>::max();
    const double scale = static_cast<double>(std::max(max_scale, 1.0f));
    const std::uint64_t node_capacity =
        static_cast<std::uint64_t>(static_cast<double>(encoded.row_nodes) * scale) + 1024;
    // Word 0 is the node counter, then one list head per (path, row), then two words per node.
    const std::uint64_t nodes_base = 1 + static_cast<std::uint64_t>(encoded.path_count) * height;
    const std::uint64_t exp_base = nodes_base + node_capacity * 2;
    const std::uint64_t work_words =
        exp_base + static_cast<std::uint64_t>(encoded.stroke_edges) * kGpuSegWords;
    const std::uint64_t output_words = static_cast<std::uint64_t>(stride) * height;
    if (node_capacity > kIndexLimit || work_words > kIndexLimit || output_words > kIndexLimit) {
        return std::nullopt;
    }
    PathRasterLayout layout;
    layout.push = {width,
                   height,
                   encoded.path_count,
                   encoded.seg_count,
                   static_cast<std::uint32_t>(node_capacity),
                   static_cast<std::uint32_t>(nodes_base),
                   encoded.fill_count,
                   static_cast<std::uint32_t>(exp_base),
                   encoded.stroke_count,
                   encoded.arc_edges,
                   stride,
                   0};
    layout.work_words = static_cast<std::size_t>(work_words);
    layout.output_words = static_cast<std::size_t>(output_words);
    return layout;
}

} // namespace tessera::rendering::path
