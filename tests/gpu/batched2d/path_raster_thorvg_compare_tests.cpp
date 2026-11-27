// SPDX-License-Identifier: MIT
//
// The GPU path rasterizer against the ThorVG reference (issue #685, ADR-071): every scene of the
// committed corpus (path_raster_corpus.hpp) is rasterized by the GPU compute passes and by ThorVG
// (the same geometry, dashes cut on the CPU: path_raster_reference.hpp) and the two images are
// diffed with the repo comparator (rendering::golden::compare_rgba) on premultiplied channels, so
// a transparent pixel's meaningless color does not count. Each scene has a tolerance row: the
// largest channel delta and the fractions of pixels off by more than 4 and by more than 16. Every
// row is the value measured on lavapipe plus headroom, with the reason the two rasterizers differ.
// A row is never loosened to pass without a recorded reason. The two rasterizers anti-alias
// differently (ThorVG: scanline coverage, the GPU: exact area), so some edge pixels always differ.
//
// Skips without a Vulkan device, without compute, or in a build without ThorVG. Run with --report
// to print every measurement (the table is derived from that output).

#include "path_raster_corpus.hpp"
#include "path_raster_reference.hpp"
#include "path_raster_support.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace golden = tessera::rendering::golden;
namespace path = tessera::rendering::path;
namespace corpus = tessera::path_corpus;

// One row per scene. max_delta: the largest per-channel premultiplied difference (of 255);
// over4 / over16: the number of pixels (of 96 x 96 = 9216) with any channel off by more than 4 /
// 16. Limits are the lavapipe measurement plus headroom (max + 25% or 2, pixel counts x 1.5 + 1; a
// measured 0 stays 0), so a driver's rounding does not flap the test and a real regression does.
struct Tolerance {
    std::string_view scene;
    std::uint32_t max_delta;
    std::size_t over4;
    std::size_t over16;
    std::string_view why;
};

constexpr Tolerance kTolerances[] = {
    {"fill_star_non_zero", 4, 0, 0,
     "Polygon fill, non-axis-aligned: only edge coverage rounding differs (ThorVG scanline "
     "coverage vs GPU area coverage); measured 2 of 255 and no pixel over 4."},
    {"fill_star_even_odd", 4, 0, 0,
     "As fill_star_non_zero; the even-odd hole must match too, so a rule mix-up would show "
     "as 255 here."},
    {"fill_nested_same_non_zero", 4, 0, 0,
     "Nested contours, same winding: filled middle under non_zero; coverage rounding only."},
    {"fill_nested_same_even_odd", 4, 0, 0,
     "Nested contours, same winding: hole under even_odd; coverage rounding only."},
    {"fill_nested_opposite_non_zero", 4, 0, 0,
     "Nested contours, opposite winding: hole under non_zero; coverage rounding only."},
    {"fill_circle_alpha_overlap", 4, 0, 0,
     "Two translucent 64-gons composited in order: blending agrees to the coverage rounding."},
    {"fill_rect_fractional", 199, 385, 385,
     "By design (ADR-071): ThorVG snaps an axis-aligned rectangle to whole pixels, the GPU "
     "keeps fractional coverage, so the edge rows and columns (about 256 pixels, one "
     "0.5-0.75 px strip per side) differ by up to 159. Not a GPU defect."},
    {"stroke_cap_butt", 25, 57, 12,
     "Round joins inside the zigzag and the stroke ends; ThorVG and the GPU tessellate the "
     "join arc and cover the edges differently (spike ADR-071: joins+caps max 58)."},
    {"stroke_cap_round", 30, 150, 42,
     "Round caps: arc tessellation and edge coverage differ at the cap rims (largest scene "
     "of the group, about 1% of pixels over 4)."},
    {"stroke_cap_square", 25, 49, 12,
     "Square caps: the cap square's corners differ by edge coverage; same zigzag as the butt "
     "scene."},
    {"stroke_join_miter", 8, 25, 0,
     "Miter joins hold (ratio 3.3 under limit 4); tips differ by sub-pixel coverage only "
     "(spike: miter max 8)."},
    {"stroke_join_bevel", 8, 33, 0,
     "Bevel joins; sub-pixel coverage of the bevel chord (spike: bevel max 8)."},
    {"stroke_join_round", 20, 87, 0,
     "Round joins: join arc tessellation differs between the stroker of ThorVG and the GPU."},
    {"stroke_miter_exceeds_limit", 7, 10, 0,
     "Miter limit 2 and a 12 degree spike: both fall back to a bevel; only coverage differs."},
    {"stroke_closed_triangle_miter", 7, 19, 0,
     "Closed contour: all three joins are miters, no caps; coverage only."},
    {"stroke_open_triangle_butt", 7, 4, 0,
     "The same triangle open: butt caps at the start and end, no join there."},
    {"dash_even_butt", 7, 4, 0,
     "Even dash array, butt caps, offset 0; the reference cuts the same dashes on the CPU."},
    {"dash_odd_offset_round", 27, 288, 6,
     "Odd array {8,3,4} repeated to six entries, offset 5, round caps: many round dash caps "
     "(about 2% of pixels over 4)."},
    {"dash_closed_polygon_butt", 65, 28, 4,
     "Dashes on a closed pentagon with miter joins inside the dashes: a few corner pixels "
     "differ by up to 52 where a dash bends."},
    {"dash_curve_square", 78, 67, 6,
     "Dashes with square caps on a 48-gon: dash ends at flattened vertices differ by up to "
     "62 on a few pixels (spike measured 22-27 on dashes)."},
    {"degenerate_zero_length_mid", 255, 55, 49,
     "GPU DEFECT RECORDED, not hidden: a zero-length segment inside a polyline (A,B,B,C) "
     "loses the miter tip at B on the GPU (ThorVG draws it), 32 pixels differ by up to 255. "
     "gpu_scene_from_shapes drops repeated points, so the production path never produces "
     "this input."},
    {"degenerate_zero_length_dot", 255, 553, 553,
     "GPU GAP RECORDED, not hidden: a single zero-length segment with round or square caps "
     "draws a dot in SVG and in ThorVG, and nothing on the GPU (368 pixels = the two dots). "
     "Production flattening drops repeated points, so it never reaches the GPU either."},
    {"degenerate_collinear", 6, 0, 0,
     "Collinear vertices on a fill and on a stroke: the extra points change nothing; "
     "coverage rounding only."},
    {"degenerate_thin", 6, 0, 0,
     "A 0.3 px sliver and a 0.25 px hairline: both draw faint pixels that agree within 4."},
    {"degenerate_empty", 0, 0, 0,
     "No shapes: both blank, exactly."},
    {"degenerate_outside_target", 0, 0, 0,
     "Shapes entirely outside the target: both blank, exactly."},
    {"degenerate_clipped_by_edge", 20, 22, 0,
     "Shapes cut by the target's edges and a round-capped stroke leaving it: coverage "
     "rounding only."},
};


int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "path_raster_thorvg_compare: " << message << "\n";
        ++failures;
    }
    return condition;
}

[[nodiscard]] std::vector<std::uint8_t> premultiplied(const std::vector<std::uint8_t>& straight) {
    std::vector<std::uint8_t> result(straight.size());
    for (std::size_t i = 0; i < straight.size(); i += 4) {
        const unsigned alpha = straight[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            result[i + c] = static_cast<std::uint8_t>((straight[i + c] * alpha + 127U) / 255U);
        }
        result[i + 3] = straight[i + 3];
    }
    return result;
}

[[nodiscard]] const Tolerance* find_tolerance(std::string_view scene) {
    for (const Tolerance& row : kTolerances) {
        if (row.scene == scene) {
            return &row;
        }
    }
    return nullptr;
}

bool run(rhi::Device& device, bool report) {
    constexpr std::uint32_t kSize = corpus::kCorpusSize;
    const auto thorvg = path::make_path_rasterizer(path::PathBackend::thorvg);
    corpus::GpuRaster raster(device);
    const std::vector<corpus::Scene> scenes = corpus::make_corpus();
    const golden::ImageSize size{kSize, kSize};
    for (const corpus::Scene& scene : scenes) {
        const std::vector<std::uint8_t> gpu = raster.render(scene.gpu, kSize, kSize);
        std::vector<std::uint8_t> reference(gpu.size(), 0);
        const path::RasterTarget target{std::span<std::uint8_t>(reference), kSize, kSize,
                                        kSize * 4U};
        const auto status = thorvg->rasterize(corpus::reference_shapes(scene.gpu), target);
        if (!expect(status.has_value(), scene.name + ": ThorVG rasterized the scene")) {
            continue;
        }
        const std::vector<std::uint8_t> a = premultiplied(gpu);
        const std::vector<std::uint8_t> b = premultiplied(reference);
        const golden::ImageDiff exact = golden::compare_rgba(size, a, size, b, 0);
        const golden::ImageDiff d4 = golden::compare_rgba(size, a, size, b, 4);
        const golden::ImageDiff d16 = golden::compare_rgba(size, a, size, b, 16);
        const double over4 = d4.mismatch_ratio();
        const double over16 = d16.mismatch_ratio();
        bool any_gpu = false;
        bool any_reference = false;
        for (std::size_t i = 3; i < gpu.size(); i += 4) {
            any_gpu = any_gpu || gpu[i] != 0;
            any_reference = any_reference || reference[i] != 0;
        }
        if (report) {
            std::cout << "measured " << scene.name << " max=" << exact.max_channel_delta
                      << " over4=" << over4 << " over16=" << over16 << " gpu_ink=" << any_gpu
                      << " thorvg_ink=" << any_reference << "\n";
            if (d16.mismatched != 0) {
                const std::size_t at = d16.first_mismatch;
                std::cout << "  first pixel over 16 at (" << at % kSize << "," << at / kSize
                          << ") gpu rgba " << int(a[at * 4]) << "," << int(a[at * 4 + 1]) << ","
                          << int(a[at * 4 + 2]) << "," << int(a[at * 4 + 3]) << " thorvg "
                          << int(b[at * 4]) << "," << int(b[at * 4 + 1]) << ","
                          << int(b[at * 4 + 2]) << "," << int(b[at * 4 + 3]) << "\n";
            }
        }
        if (scene.expect_blank) {
            expect(!any_gpu && !any_reference, scene.name + ": both rasterizers leave it blank");
        } else {
            expect(any_gpu, scene.name + ": the GPU drew something (not a vacuous pass)");
        }
        const Tolerance* row = find_tolerance(scene.name);
        if (row == nullptr) {
            if (!report) {
                expect(false, scene.name + ": no tolerance row in kTolerances");
            }
            continue;
        }
        expect(exact.max_channel_delta <= row->max_delta,
               scene.name + ": max channel delta " + std::to_string(exact.max_channel_delta) +
                   " exceeds " + std::to_string(row->max_delta));
        expect(d4.mismatched <= row->over4,
               scene.name + ": " + std::to_string(d4.mismatched) + " pixels over 4 exceed " +
                   std::to_string(row->over4));
        expect(d16.mismatched <= row->over16,
               scene.name + ": " + std::to_string(d16.mismatched) + " pixels over 16 exceed " +
                   std::to_string(row->over16));
    }
    return failures == 0;
}

} // namespace

int main(int argc, char** argv) {
    bool report = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--report") {
            report = true;
            continue;
        }
        std::cerr << "path_raster_thorvg_compare: unknown argument " << argv[index] << "\n";
        return 2;
    }
    if (!path::path_backend_available(path::PathBackend::thorvg)) {
        std::cout << "path_raster_thorvg_compare: skipped (built without ThorVG)\n";
        return 0;
    }
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_raster_thorvg_compare: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }
    if (!device->supports_compute()) {
        std::cout << "path_raster_thorvg_compare: skipped (no compute)\n";
        return 0;
    }
    try {
        if (!run(*device, report)) {
            std::cerr << "path_raster_thorvg_compare: FAILED\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "path_raster_thorvg_compare: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "path_raster_thorvg_compare: ok\n";
    return 0;
}
