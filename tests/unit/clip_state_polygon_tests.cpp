// SPDX-License-Identifier: MIT
//
// ClipStateTable polygon pooling (ADR-068 slice 5, issue #673): the per-frame vertex pool, its
// deduplication, and the over-limit fallback to the bounding box.

#include "tessera/rendering/gpu/clip_state_table.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace clip = tessera::rendering::gpu::clip;
namespace gpu = tessera::rendering::gpu;
namespace rhi = tessera::rendering::rhi;

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "clip_state_polygon: " << message << "\n";
        ++g_failures;
    }
}

constexpr rhi::Scissor kBase{0, 0, 128, 128};

// A polygon of `count` vertices on a ring whose bounding box is [x, x + 40] x [10, 50].
std::vector<clip::ClipVertex> ring(std::uint32_t count, float x) {
    std::vector<clip::ClipVertex> vertices;
    vertices.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        // Alternate between the four box corners so the box is exactly [x, x+40] x [10, 50].
        const float vx = (index % 2 == 0 ? 0.0f : 40.0f) + x;
        const float vy = (index % 4 < 2 ? 10.0f : 50.0f);
        vertices.push_back({vx, vy});
    }
    return vertices;
}

clip::ShapeClip shape_of(const std::vector<clip::ClipVertex>& vertices) {
    clip::ShapeClip shape;
    shape.record.kind = static_cast<std::uint32_t>(clip::RecordKind::Polygon);
    float min_x = vertices.empty() ? 0.0f : vertices.front().x;
    float max_x = min_x;
    float min_y = vertices.empty() ? 0.0f : vertices.front().y;
    float max_y = min_y;
    for (const clip::ClipVertex& vertex : vertices) {
        min_x = std::min(min_x, vertex.x);
        max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y);
        max_y = std::max(max_y, vertex.y);
    }
    shape.record.x = min_x;
    shape.record.y = min_y;
    shape.record.width = max_x - min_x;
    shape.record.height = max_y - min_y;
    shape.record.vertex_count = static_cast<std::uint32_t>(vertices.size());
    shape.vertices = vertices;
    return shape;
}

void test_pool_and_dedupe() {
    gpu::ClipStateTable table;
    table.reset();
    const std::vector<clip::ClipVertex> a = ring(4, 10.0f);
    const std::vector<clip::ClipVertex> b = ring(4, 60.0f);

    const auto first = table.push_shape(shape_of(a), kBase);
    expect(table.set(first.set).size() == 1, "a polygon is one record");
    const clip::ClipRecord record = table.set(first.set).front();
    expect(record.kind == static_cast<std::uint32_t>(clip::RecordKind::Polygon) &&
               record.first_vertex == 0 && record.vertex_count == 4,
           "the record names its vertices in the pool");
    expect(table.flat_vertices().size() == 4, "the pool holds the polygon's vertices");
    expect(first.scissor.x == 10 && first.scissor.width == 40 && first.scissor.height == 40,
           "the scissor is the polygon's bounding box");

    const auto second = table.push_shape(shape_of(b), first.scissor);
    expect(table.flat_vertices().size() == 8 && table.set(second.set).back().first_vertex == 4,
           "a different polygon appends to the pool");

    // The same polygon again shares its pooled run, so the pool does not grow.
    (void)table.pop(first.scissor);
    (void)table.pop(kBase);
    const auto again = table.push_shape(shape_of(a), kBase);
    expect(table.flat_vertices().size() == 8, "an identical polygon reuses its pooled vertices");
    expect(again.set == first.set, "an identical polygon interns to the same set");
    expect(table.shape_fallbacks() == 0, "nothing fell back");

    table.reset();
    expect(table.flat_vertices().empty() && table.shape_fallbacks() == 0,
           "a new frame empties the pool and the counter");
}

void test_polygon_over_the_vertex_limit() {
    gpu::ClipStateTable table;
    table.reset();
    const std::vector<clip::ClipVertex> big = ring(clip::kMaxPolygonVertices + 1, 10.0f);
    const auto update = table.push_shape(shape_of(big), kBase);
    const clip::ClipRecord record = table.set(update.set).front();
    expect(record.kind == static_cast<std::uint32_t>(clip::RecordKind::RoundedRect),
           "a polygon over the per-polygon limit degrades to its bounding box");
    expect(record.x == 10.0f && record.width == 40.0f && record.height == 40.0f,
           "the fallback record is the bounding box");
    expect(table.shape_fallbacks() == 1 && table.flat_vertices().empty(),
           "the fallback is counted and pools nothing");

    const std::vector<clip::ClipVertex> exact = ring(clip::kMaxPolygonVertices, 10.0f);
    table.reset();
    const auto fits = table.push_shape(shape_of(exact), kBase);
    expect(table.set(fits.set).front().kind ==
                   static_cast<std::uint32_t>(clip::RecordKind::Polygon) &&
               table.shape_fallbacks() == 0,
           "a polygon of exactly the limit stays a polygon");
}

void test_frame_pool_limit() {
    gpu::ClipStateTable table;
    table.reset();
    constexpr std::uint32_t kFits = clip::kMaxClipVertices / clip::kMaxPolygonVertices;
    rhi::Scissor scissor = kBase;
    for (std::uint32_t index = 0; index < kFits; ++index) {
        // Distinct offsets so deduplication cannot hide the growth.
        scissor =
            table
                .push_shape(shape_of(ring(clip::kMaxPolygonVertices, static_cast<float>(index))),
                            scissor)
                .scissor;
    }
    expect(table.flat_vertices().size() == clip::kMaxClipVertices && table.shape_fallbacks() == 0,
           "the pool fills to its frame limit");
    const auto over = table.push_shape(
        shape_of(ring(clip::kMaxPolygonVertices, static_cast<float>(kFits))), scissor);
    expect(table.shape_fallbacks() == 1 && table.flat_vertices().size() == clip::kMaxClipVertices,
           "a polygon past the frame limit degrades and is counted");
    expect(table.set(over.set).back().kind ==
               static_cast<std::uint32_t>(clip::RecordKind::RoundedRect),
           "the degraded record is a bounding box");
}

void test_degenerate() {
    gpu::ClipStateTable table;
    table.reset();
    const std::vector<clip::ClipVertex> line{{10.0f, 10.0f}, {50.0f, 50.0f}};
    const auto update = table.push_shape(shape_of(line), kBase);
    const clip::ClipRecord record = table.set(update.set).front();
    expect(record.width == 0.0f && record.height == 0.0f && update.scissor.width == 0,
           "a polygon with fewer than three vertices clips everything away");
    expect(table.flat_vertices().empty() && table.shape_fallbacks() == 0,
           "a degenerate polygon pools nothing and is not a fallback");
}

void test_shapes_nest_with_rounded_clips() {
    gpu::ClipStateTable table;
    table.reset();
    const auto outer = table.push(
        clip::make_rounded_rect(rhi::Rect{0.0f, 0.0f, 100.0f, 100.0f}, 8.0f, 8.0f, 8.0f, 8.0f),
        kBase);
    const auto inner = table.push_shape(shape_of(ring(4, 20.0f)), outer.scissor);
    expect(table.set(inner.set).size() == 2, "a polygon nests under a rounded clip");
    expect(table.set(inner.set).front().kind ==
                   static_cast<std::uint32_t>(clip::RecordKind::RoundedRect) &&
               table.set(inner.set).back().kind ==
                   static_cast<std::uint32_t>(clip::RecordKind::Polygon),
           "records keep stack order");
    const auto popped = table.pop(inner.scissor);
    expect(popped.set == outer.set, "popping the polygon restores the rounded clip's set");
}

} // namespace

int main() {
    test_pool_and_dedupe();
    test_polygon_over_the_vertex_limit();
    test_frame_pool_limit();
    test_degenerate();
    test_shapes_nest_with_rounded_clips();
    if (g_failures != 0) {
        std::cerr << "clip_state_polygon: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "clip_state_polygon: ok\n";
    return 0;
}
