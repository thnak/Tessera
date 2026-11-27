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

// One row per scene. max_delta: the largest per-channel premultiplied difference; over4/over16:
// the fraction of pixels with any channel off by more than 4 / 16 of 255.
struct Tolerance {
    std::string_view scene;
    std::uint32_t max_delta;
    double over4;
    double over16;
    std::string_view why;
};

// UNMEASURED rows are filled in from a --report run.
constexpr Tolerance kTolerances[] = {
    {"UNMEASURED", 255, 1.0, 1.0, ""},
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
        expect(over4 <= row->over4, scene.name + ": fraction over 4 " + std::to_string(over4) +
                                        " exceeds " + std::to_string(row->over4));
        expect(over16 <= row->over16, scene.name + ": fraction over 16 " + std::to_string(over16) +
                                          " exceeds " + std::to_string(row->over16));
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
