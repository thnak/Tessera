// SPDX-License-Identifier: MIT
//
// Shared helpers of the path-texture draw tests: a solid-color PathRasterizer, box shape/key
// builders and SVG fixture lookup. Header-only (inline) so the CTest binary and the manual
// `--measure` tool share one definition.

#pragma once

#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::test::path_draw {

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

inline constexpr std::uint32_t kBox = 64;

[[nodiscard]] inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "path_texture_draw: " << message << "\n";
    }
    return condition;
}

// Fills every pixel with one straight-alpha color: enough to verify upload bytes, draw plumbing and
// the blend equation without depending on ThorVG (optional) or on a specific rasterizer.
class SolidRasterizer final : public path::PathRasterizer {
public:
    explicit SolidRasterizer(path::Rgba8 color)
        : color_(color) {}

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "solid";
    }

    [[nodiscard]] std::expected<path::RasterStats, path::RasterError>
    rasterize(std::span<const path::PathShape> shapes, const path::RasterTarget& target) override {
        ++calls;
        if (!target.valid()) {
            return std::unexpected(path::RasterError::invalid_target);
        }
        for (std::uint32_t y = 0; y < target.height; ++y) {
            std::uint8_t* row =
                target.pixels.data() + static_cast<std::size_t>(y) * target.stride_bytes;
            for (std::uint32_t x = 0; x < target.width; ++x) {
                row[x * 4U + 0U] = color_.r;
                row[x * 4U + 1U] = color_.g;
                row[x * 4U + 2U] = color_.b;
                row[x * 4U + 3U] = color_.a;
            }
        }
        return path::RasterStats{static_cast<std::uint32_t>(shapes.size())};
    }

    std::uint32_t calls = 0;

private:
    path::Rgba8 color_;
};

[[nodiscard]] inline path::PathShape box_shape() {
    path::Path path;
    path.segments.push_back({path::SegmentKind::move_to, {0.0f, 0.0f}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {static_cast<float>(kBox), 0.0f}, {}, {}});
    path.segments.push_back(
        {path::SegmentKind::line_to, {static_cast<float>(kBox), static_cast<float>(kBox)}, {}, {}});
    path.segments.push_back({path::SegmentKind::line_to, {0.0f, static_cast<float>(kBox)}, {}, {}});
    path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    path::PaintStyle paint;
    paint.fill =
        path::Rgba8{0, 0, 0, 255}; // the rasterizer overrides pixels; paint is the key input
    return path::PathShape{std::move(path), paint};
}

[[nodiscard]] inline path::PathTextureKey box_key(std::uint64_t id, std::uint32_t scale_milli,
                                                  const path::PathShape& shape) {
    return path::make_path_texture_key(id, 1, kBox, kBox, scale_milli, shape);
}

// Resolves an SVG fixture without a compile definition (shared test registries stay untouched).
// An absolute __FILE__ works from any build directory; when the build tree lives inside the source
// tree the compiler records a relative source path, so fall back to walking up from the working
// directory (CTest runs in <build>/tests/gpu, whose ancestors include the repository root).
[[nodiscard]] inline std::filesystem::path find_svg_fixture(std::string_view name) {
#ifdef TESSERA_SVG_FIXTURE_DIR
    {
        const std::filesystem::path candidate =
            std::filesystem::path(TESSERA_SVG_FIXTURE_DIR) / std::string(name);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
#endif
    // Fallbacks for builds that do not pass the compile definition: an absolute __FILE__ works from
    // any build directory, and when the compiler records a relative source path the test walks up
    // from the working directory (CTest runs in <build>/tests/gpu).
    const std::filesystem::path source(__FILE__);
    if (source.is_absolute()) {
        const std::filesystem::path candidate = source.parent_path().parent_path().parent_path() /
                                                "fixtures" / "svg" / std::string(name);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    std::filesystem::path directory = std::filesystem::current_path();
    for (int depth = 0; depth < 8 && !directory.empty(); ++depth) {
        const std::filesystem::path candidate =
            directory / "tests" / "fixtures" / "svg" / std::string(name);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        if (directory == directory.root_path()) {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

[[nodiscard]] inline std::vector<std::uint8_t> read_svg_fixture(std::string_view name) {
    const std::filesystem::path path = find_svg_fixture(name);
    if (path.empty()) {
        std::cerr << "path_texture_draw: cannot locate fixture " << name << "\n";
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "path_texture_draw: cannot open " << path.string() << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] inline std::uint64_t retained_document_bytes(const path::SvgDocument& document) {
    std::uint64_t bytes = 0;
    for (const path::PathShape& shape : document.shapes) {
        bytes += sizeof(path::PathShape);
        bytes +=
            static_cast<std::uint64_t>(shape.path.segments.capacity()) * sizeof(path::PathSegment);
    }
    return bytes;
}

} // namespace tessera::test::path_draw
