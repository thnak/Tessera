// SPDX-License-Identifier: MIT
//
// ThorVG spike driver: rasterizes a representative icon/chart path set through the
// PathRasterizer seam, writes a PNG (and optionally the raw RGBA8 buffer), prints startup and
// first-render timings, and compares against a checked-in golden.

#include "options.hpp"
#include "scene.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace {

using tessera::rendering::golden::ImageSize;
using tessera::rendering::path::PathBackend;
using tessera::rendering::path::PathRasterizer;
using tessera::rendering::path::RasterError;
using tessera::rendering::path::RasterStats;
using tessera::rendering::path::RasterTarget;
using tessera::rendering::path::Rgba8;

constexpr Rgba8 kBackground{16, 22, 34, 255};

[[nodiscard]] std::string_view raster_error_name(RasterError error) noexcept {
    switch (error) {
    case RasterError::invalid_target:
        return "invalid_target";
    case RasterError::invalid_path:
        return "invalid_path";
    case RasterError::invalid_paint:
        return "invalid_paint";
    case RasterError::unsupported_backend:
        return "unsupported_backend";
    case RasterError::backend_failure:
        return "backend_failure";
    }
    return "unknown";
}

[[nodiscard]] std::string_view svg_error_name(tessera::rendering::path::SvgError error) noexcept {
    using tessera::rendering::path::SvgError;
    switch (error) {
    case SvgError::invalid_argument:
        return "invalid_argument";
    case SvgError::input_too_large:
        return "input_too_large";
    case SvgError::malformed:
        return "malformed";
    case SvgError::unsupported:
        return "unsupported";
    case SvgError::limit_exceeded:
        return "limit_exceeded";
    case SvgError::backend_unavailable:
        return "backend_unavailable";
    case SvgError::backend_failure:
        return "backend_failure";
    }
    return "unknown";
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

// The seam has no transform stack; the spike scales a decoded document for 2x rasterization.
void scale_shapes(std::vector<tessera::rendering::path::PathShape>& shapes, float scale) {
    using tessera::rendering::path::PathSegment;
    for (tessera::rendering::path::PathShape& shape : shapes) {
        for (PathSegment& segment : shape.path.segments) {
            segment.p0 = {segment.p0.x * scale, segment.p0.y * scale};
            segment.p1 = {segment.p1.x * scale, segment.p1.y * scale};
            segment.p2 = {segment.p2.x * scale, segment.p2.y * scale};
        }
        if (shape.paint.stroke.has_value()) {
            shape.paint.stroke->width *= scale;
        }
    }
}

// Approximate retained size of a decoded document: vector objects plus allocated segment storage.
[[nodiscard]] std::uint64_t document_bytes(const tessera::rendering::path::SvgDocument& document) {
    std::uint64_t bytes = 0;
    for (const tessera::rendering::path::PathShape& shape : document.shapes) {
        bytes += sizeof(tessera::rendering::path::PathShape);
        bytes += static_cast<std::uint64_t>(shape.path.segments.capacity()) *
                 sizeof(tessera::rendering::path::PathSegment);
    }
    return bytes;
}

void fill_background(std::span<std::uint8_t> pixels, std::uint32_t width, std::uint32_t height,
                     std::uint32_t stride_bytes, Rgba8 color) {
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * stride_bytes;
        for (std::uint32_t x = 0; x < width; ++x) {
            row[x * 4U + 0U] = color.r;
            row[x * 4U + 1U] = color.g;
            row[x * 4U + 2U] = color.b;
            row[x * 4U + 3U] = color.a;
        }
    }
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] double elapsed_ms(std::chrono::steady_clock::time_point from,
                                std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

[[nodiscard]] bool write_raw(const std::string& path, std::span<const std::uint8_t> pixels) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "cannot open " << path << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        return false;
    }
    return true;
}

[[nodiscard]] int compare_golden(const std::string& path, std::uint32_t width, std::uint32_t height,
                                 std::span<const std::uint8_t> pixels, std::uint32_t tolerance,
                                 double max_mismatch) {
    int golden_width = 0;
    int golden_height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &golden_width, &golden_height, &channels, 4);
    if (decoded == nullptr) {
        std::cerr << "cannot decode golden: " << path << "\n";
        return 1;
    }

    const auto expected_size =
        static_cast<std::size_t>(golden_width) * static_cast<std::size_t>(golden_height) * 4U;
    const std::span<const std::uint8_t> expected(decoded, expected_size);
    const auto diff = tessera::rendering::golden::compare_rgba(
        ImageSize{width, height}, pixels,
        ImageSize{static_cast<std::uint32_t>(golden_width),
                  static_cast<std::uint32_t>(golden_height)},
        expected, tolerance);
    const bool pass = tessera::rendering::golden::within_tolerance(diff, max_mismatch);

    std::cout << "compare: " << (pass ? "match" : "MISMATCH") << " (" << diff.mismatched << "/"
              << diff.pixels << " pixels differ, max channel delta " << diff.max_channel_delta
              << ") against " << path << "\n";
    stbi_image_free(decoded);
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const auto process_start = std::chrono::steady_clock::now();

    tessera::poc::thorvg_spike::Options options;
    if (!tessera::poc::thorvg_spike::parse_options(argc, argv, options)) {
        tessera::poc::thorvg_spike::print_usage();
        return 2;
    }
    if (options.help) {
        tessera::poc::thorvg_spike::print_usage();
        return 0;
    }
    if (options.width == 0U || options.height == 0U) {
        std::cerr << "width and height must be positive\n";
        return 2;
    }

    PathBackend backend = PathBackend::thorvg;
    if (options.backend == "null") {
        backend = PathBackend::null_backend;
    } else if (options.backend != "thorvg") {
        std::cerr << "unknown backend: " << options.backend << "\n";
        return 2;
    }
    if (!tessera::rendering::path::path_backend_available(backend)) {
        std::cerr << "backend '" << options.backend
                  << "' is not compiled in (configure with -DTESSERA_ENABLE_THORVG=ON)\n";
        return 2;
    }

    const std::unique_ptr<PathRasterizer> rasterizer =
        tessera::rendering::path::make_path_rasterizer(backend);
    if (rasterizer == nullptr) {
        std::cerr << "cannot create backend '" << options.backend << "'\n";
        return 2;
    }
    const auto rasterizer_ready = std::chrono::steady_clock::now();

    std::uint32_t width = options.width;
    std::uint32_t height = options.height;
    std::vector<tessera::rendering::path::PathShape> shapes;
    double svg_load_ms = 0.0;
    if (!options.svg.empty()) {
        const auto load_start = std::chrono::steady_clock::now();
        const auto bytes = read_file(options.svg);
        if (bytes.empty()) {
            std::cerr << "cannot read " << options.svg << "\n";
            return 2;
        }
        if (options.svg_measure) {
            // Document-cache measurement: first acquire decodes, cached acquires are lookups.
            tessera::rendering::path::SvgDocumentCache cache;
            const auto key = tessera::rendering::path::make_svg_document_key(bytes);
            const auto first_start = std::chrono::steady_clock::now();
            const auto first = cache.acquire(key, std::span<const std::uint8_t>(bytes));
            const auto first_end = std::chrono::steady_clock::now();
            if (!first.has_value()) {
                std::cerr << "svg decode failed: " << svg_error_name(first.error()) << "\n";
                return 3;
            }
            constexpr int kCachedAcquires = 10000;
            double cached_total_ms = 0.0;
            for (int i = 0; i < kCachedAcquires; ++i) {
                const auto begin = std::chrono::steady_clock::now();
                const auto hit = cache.acquire(key, std::span<const std::uint8_t>());
                const auto end = std::chrono::steady_clock::now();
                cached_total_ms += elapsed_ms(begin, end);
                if (!hit.has_value()) {
                    std::cerr << "cached acquire failed\n";
                    return 3;
                }
            }
            const auto& document = *first.value();
            const std::uint64_t texture_bytes =
                static_cast<std::uint64_t>(document.width) * document.height * 4U;
            std::cout << std::fixed << std::setprecision(4)
                      << "svg_cache: first_acquire_ms=" << elapsed_ms(first_start, first_end)
                      << " cached_acquire_ms_avg=" << (cached_total_ms / kCachedAcquires)
                      << " doc_shapes=" << document.shapes.size()
                      << " doc_segments=" << cache.segment_count()
                      << " doc_bytes=" << document_bytes(document)
                      << " texture_bytes_1x=" << texture_bytes
                      << " decode_count=" << cache.stats().decodes << "\n";
            return 0;
        }
        auto document = tessera::rendering::path::load_svg(
            std::span<const std::uint8_t>(bytes.data(), bytes.size()));
        if (!document.has_value()) {
            std::cerr << "svg load failed: " << svg_error_name(document.error()) << "\n";
            return 3;
        }
        shapes = std::move(document->shapes);
        width = document->width;
        height = document->height;
        if (options.svg_scale != 1.0) {
            scale_shapes(shapes, static_cast<float>(options.svg_scale));
            width = static_cast<std::uint32_t>(
                std::ceil(static_cast<double>(width) * options.svg_scale));
            height = static_cast<std::uint32_t>(
                std::ceil(static_cast<double>(height) * options.svg_scale));
        }
        svg_load_ms = elapsed_ms(load_start, std::chrono::steady_clock::now());
    } else {
        shapes = tessera::poc::thorvg_spike::build_icon_chart_scene(static_cast<float>(width),
                                                                    static_cast<float>(height));
    }

    const auto stride_bytes = static_cast<std::uint32_t>(static_cast<std::size_t>(width) * 4U);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride_bytes) * height);
    fill_background(pixels, width, height, stride_bytes, kBackground);

    const RasterTarget target{pixels, width, height, stride_bytes};

    const auto render_start = std::chrono::steady_clock::now();
    const std::expected<RasterStats, RasterError> stats = rasterizer->rasterize(shapes, target);
    const auto render_end = std::chrono::steady_clock::now();
    if (!stats.has_value()) {
        std::cerr << "rasterize failed: " << raster_error_name(stats.error()) << "\n";
        return 3;
    }

    const std::uint64_t hash = fnv1a64(pixels);

    int exit_code = 0;
    if (!options.raw.empty() && !write_raw(options.raw, pixels)) {
        exit_code = 4;
    }
    if (stbi_write_png(options.out.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                       pixels.data(), static_cast<int>(stride_bytes)) == 0) {
        std::cerr << "cannot write " << options.out << "\n";
        return 4;
    }
    const auto process_end = std::chrono::steady_clock::now();

    if (!options.svg.empty()) {
        std::cout << "svg: " << options.svg << ", raster " << width << "x" << height << "\n";
    }
    std::cout << "backend: " << rasterizer->backend_name() << "\n"
              << "canvas: " << width << "x" << height << ", stride " << stride_bytes << " bytes, "
              << pixels.size() << " pixel bytes\n"
              << "shapes: " << stats->shapes << "\n"
              << "fnv1a64: 0x" << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec
              << std::setfill(' ') << "\n";

    if (!options.svg.empty()) {
        std::cout << std::fixed << std::setprecision(3) << "svg_load_ms: " << svg_load_ms << "\n";
    }
    std::cout << std::fixed << std::setprecision(3)
              << "startup_ms: " << elapsed_ms(process_start, rasterizer_ready) << "\n"
              << "first_render_ms: " << elapsed_ms(render_start, render_end) << "\n"
              << "total_ms: " << elapsed_ms(process_start, process_end) << "\n"
              << "wrote: " << options.out << "\n";

    if (!options.compare.empty()) {
        const int compare_code = compare_golden(options.compare, width, height, pixels,
                                                options.tolerance, options.max_mismatch);
        if (compare_code != 0) {
            exit_code = compare_code;
        }
    }
    return exit_code;
}
