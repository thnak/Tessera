// SPDX-License-Identifier: MIT
//
// Manual measurement tool (not part of the CTest run): upload cost for a 512x512 path texture,
// steady-state cache-hit cost and SVG decode/texture cost on a real Vulkan device.
// Split from path_texture_draw_tests.cpp (formerly `path_texture_draw_tests --measure`).

#include "path_texture_draw_support.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>

namespace {

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

using tessera::test::path_draw::box_key;
using tessera::test::path_draw::box_shape;
using tessera::test::path_draw::kBox;
using tessera::test::path_draw::read_svg_fixture;
using tessera::test::path_draw::retained_document_bytes;
using tessera::test::path_draw::SolidRasterizer;

// Manual measurement mode (`--measure`), not part of the CTest run: upload cost for a 512x512 path
// texture and steady-state cache-hit cost on a real device. The rasterizer is a solid fill, so the
// miss number isolates rasterize + atlas-free upload into the RHI.
void measure_upload_and_hits(rhi::Device& device) {
    constexpr std::uint32_t kSize = 512;
    path::Path path;
    path.segments.push_back({path::SegmentKind::move_to, {0.0f, 0.0f}, {}, {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {static_cast<float>(kSize), 0.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to,
                             {static_cast<float>(kSize), static_cast<float>(kSize)},
                             {},
                             {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {0.0f, static_cast<float>(kSize)}, {}, {}});
    path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    path::PaintStyle paint;
    paint.fill = path::Rgba8{10, 200, 60, 255};
    const path::PathShape shape{std::move(path), paint};

    SolidRasterizer rasterizer{path::Rgba8{10, 200, 60, 255}};
    path::PathTextureCacheConfig config;
    config.max_entries = 8;
    config.max_bytes = 64ULL * 1024ULL * 1024ULL;
    path::PathTextureCache cache(device, rasterizer, config);

    const auto key = path::make_path_texture_key(9, 1, kSize, kSize, 1000, shape);
    const auto start = std::chrono::steady_clock::now();
    const auto first = cache.acquire(key, shape);
    const auto first_done = std::chrono::steady_clock::now();
    if (!first.has_value()) {
        std::cerr << "path_texture_draw: measurement upload failed\n";
        return;
    }
    const double first_ms = std::chrono::duration<double, std::milli>(first_done - start).count();

    constexpr int kHitIterations = 10000;
    double hit_total_ms = 0.0;
    for (int i = 0; i < kHitIterations; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto hit = cache.acquire(key, shape);
        const auto end = std::chrono::steady_clock::now();
        hit_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!hit.has_value()) {
            std::cerr << "path_texture_draw: measurement hit failed\n";
            return;
        }
    }

    constexpr int kUploadIterations = 10;
    double upload_total_ms = 0.0;
    for (int i = 0; i < kUploadIterations; ++i) {
        cache.invalidate_all();
        const auto begin = std::chrono::steady_clock::now();
        const auto uploaded = cache.acquire(key, shape);
        const auto end = std::chrono::steady_clock::now();
        upload_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!uploaded.has_value()) {
            std::cerr << "path_texture_draw: measurement re-upload failed\n";
            return;
        }
    }

    std::cout << std::fixed << std::setprecision(4)
              << "path_texture_draw: measure 512x512 first_acquire_ms=" << first_ms
              << " upload_again_ms_avg=" << (upload_total_ms / kUploadIterations)
              << " hit_ms_avg=" << (hit_total_ms / kHitIterations)
              << " texture_bytes=" << (512ULL * 512ULL * 4ULL) << "\n";
}

// Manual measurement for the decoded-document pipeline: document-cache first vs cached acquire and
// document-texture first upload vs cached acquire, plus retained document memory vs texture memory.
void measure_svg_document(rhi::Device& device) {
    const auto bytes = read_svg_fixture("icon_shapes.svg");
    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (bytes.empty() || !path::svg_backend_available() || rasterizer == nullptr) {
        std::cout << "path_texture_draw: SVG measure unavailable (backend or fixture missing)\n";
        return;
    }

    path::SvgDocumentCache documents;
    const auto key = path::make_svg_document_key(std::span<const std::uint8_t>(bytes));
    const auto document_start = std::chrono::steady_clock::now();
    const auto document = documents.acquire(key, std::span<const std::uint8_t>(bytes));
    const auto document_end = std::chrono::steady_clock::now();
    if (!document.has_value()) {
        std::cout << "path_texture_draw: SVG measure decode failed\n";
        return;
    }
    const double document_first_ms =
        std::chrono::duration<double, std::milli>(document_end - document_start).count();

    constexpr int kDocumentHits = 10000;
    double document_hit_total_ms = 0.0;
    for (int i = 0; i < kDocumentHits; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto hit = documents.acquire(key, std::span<const std::uint8_t>());
        const auto end = std::chrono::steady_clock::now();
        document_hit_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!hit.has_value()) {
            std::cout << "path_texture_draw: SVG measure cache hit failed\n";
            return;
        }
    }

    const path::SvgTextureKey texture_key = path::make_svg_texture_key(9, 1, *document.value());
    path::PathTextureCache textures(device, *rasterizer, {});
    const auto texture_start = std::chrono::steady_clock::now();
    const auto uploaded = path::acquire_svg_texture(textures, texture_key, *document.value());
    const auto texture_end = std::chrono::steady_clock::now();
    if (!uploaded.has_value()) {
        std::cout << "path_texture_draw: SVG measure texture upload failed\n";
        return;
    }
    const double texture_first_ms =
        std::chrono::duration<double, std::milli>(texture_end - texture_start).count();

    constexpr int kTextureHits = 1000;
    double texture_hit_total_ms = 0.0;
    for (int i = 0; i < kTextureHits; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto hit = path::acquire_svg_texture(textures, texture_key, *document.value());
        const auto end = std::chrono::steady_clock::now();
        texture_hit_total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        if (!hit.has_value()) {
            std::cout << "path_texture_draw: SVG measure texture hit failed\n";
            return;
        }
    }

    const path::SvgDocument& decoded = *document.value();
    std::cout << std::fixed << std::setprecision(4)
              << "svg_document: first_acquire_ms=" << document_first_ms
              << " cached_acquire_ms_avg=" << (document_hit_total_ms / kDocumentHits)
              << " doc_shapes=" << decoded.shapes.size()
              << " doc_segments=" << documents.segment_count()
              << " doc_bytes=" << retained_document_bytes(decoded)
              << " first_texture_ms=" << texture_first_ms
              << " cached_texture_ms_avg=" << (texture_hit_total_ms / kTextureHits)
              << " texture_bytes_1x="
              << (static_cast<std::uint64_t>(decoded.width) * decoded.height * 4U) << "\n";
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    try {
        const auto device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
        measure_upload_and_hits(*device);
        measure_svg_document(*device);
    } catch (const std::exception& error) {
        std::cerr << "path_texture_measure: skipped (" << error.what() << ")\n";
        return 1;
    }
    return 0;
}
