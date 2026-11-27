// SPDX-License-Identifier: MIT
//
// Lavapipe goldens for the GPU path rasterizer (issue #685, ADR-071): every scene of the committed
// corpus (path_raster_corpus.hpp: each fill rule, cap, join, miter fallback, dash and degenerate
// case) rendered by the compute passes on a software Vulkan device and compared bit for bit with
// tests/golden/gpu/path_raster/<scene>.png (straight-alpha RGBA, the rasterizer's own output).
//
//  - provenance: pixels are pinned only on a software rasterizer (ADR-009); a hardware device is
//    refused with a skip, so a developer GPU can never write or verify these pixels;
//  - determinism: two renders of a scene are byte-identical before either is compared or pinned;
//  - blank scenes (empty, fully outside the target) must stay fully transparent.
//
// Regenerate on a software device only: path_raster_golden_tests --update-goldens. The ctest entry
// is registered on UNIX with a lavapipe manifest only, like the other lavapipe goldens.

#include "path_raster_corpus.hpp"
#include "path_raster_support.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_device_info.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_PATH_RASTER_GOLDEN_DIR
#error "TESSERA_PATH_RASTER_GOLDEN_DIR must point at tests/golden/gpu/path_raster"
#endif

namespace {

namespace rhi = tessera::rendering::rhi;
namespace corpus = tessera::path_corpus;
using tessera::harness::CompareMode;
using tessera::harness::ComparePolicy;
using tessera::harness::RgbaImage;

int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "path_raster_golden: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    bool update_goldens = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "path_raster_golden: unknown argument " << argv[index] << "\n";
        return 2;
    }

    std::unique_ptr<rhi::Device> device;
    try {
        rhi::DeviceDesc device_desc;
        device_desc.backend = rhi::Backend::Vulkan;
        device_desc.validation = true;
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_raster_golden: skipped (no Vulkan device: " << error.what() << ")\n";
        return 0;
    }
    const auto info = tessera::rendering::vulkan::device_info(*device);
    if (!info || !info->software()) {
        std::cout << "path_raster_golden: skipped (no software Vulkan device on this host"
                  << (info ? ": " + info->describe() : std::string()) << ")\n";
        return 0;
    }
    if (!device->supports_compute()) {
        std::cout << "path_raster_golden: skipped (no compute)\n";
        return 0;
    }
    std::cout << "path_raster_golden: device " << info->describe() << "\n";

    constexpr std::uint32_t kSize = corpus::kCorpusSize;
    const std::string dir = TESSERA_PATH_RASTER_GOLDEN_DIR;
    try {
        corpus::GpuRaster raster(*device);
        for (const corpus::Scene& scene : corpus::make_corpus()) {
            const std::vector<std::uint8_t> first = raster.render(scene.gpu, kSize, kSize);
            const std::vector<std::uint8_t> second = raster.render(scene.gpu, kSize, kSize);
            if (!expect(first == second, scene.name + ": two renders are byte-identical")) {
                continue;
            }
            bool any_ink = false;
            for (std::size_t i = 3; i < first.size(); i += 4) {
                any_ink = any_ink || first[i] != 0;
            }
            expect(any_ink != scene.expect_blank,
                   scene.name +
                       (scene.expect_blank ? ": stays fully transparent" : ": draws something"));
            RgbaImage frame;
            frame.width = kSize;
            frame.height = kSize;
            frame.rgba = first;
            const std::string file = dir + "/" + scene.name + ".png";
            if (update_goldens) {
                const auto png = tessera::harness::encode_png(frame);
                std::filesystem::create_directories(dir);
                if (expect(png.has_value(), scene.name + ": the frame encodes as PNG") &&
                    expect(write_file(file, *png), scene.name + ": the golden was written")) {
                    std::cout << "path_raster_golden: wrote " << file << "\n";
                }
                continue;
            }
            const std::vector<std::uint8_t> bytes = read_file(file);
            if (!expect(!bytes.empty(), scene.name + ": the committed golden exists (" + file +
                                            "); regenerate with --update-goldens on a software "
                                            "device")) {
                continue;
            }
            const auto baseline = tessera::harness::decode_png(bytes);
            if (!expect(baseline.has_value() && baseline->width == kSize &&
                            baseline->height == kSize,
                        scene.name + ": the golden decodes and has the corpus extent")) {
                continue;
            }
            const auto result = tessera::harness::compare_images(CompareMode::Golden, frame,
                                                                 &*baseline, ComparePolicy{0, 0.0});
            if (!expect(result.passed(), scene.name + ": bit-identical to the golden")) {
                std::cerr << "path_raster_golden: " << scene.name
                          << " mismatched=" << result.diff.mismatched
                          << " max_delta=" << result.diff.max_channel_delta << "\n";
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "path_raster_golden: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    expect(tessera::rendering::vulkan::validation_message_count() == 0,
           "the run produced no validation messages");
    if (failures != 0) {
        std::cerr << "path_raster_golden: failed\n";
        return 1;
    }
    std::cout << "path_raster_golden: ok\n";
    return 0;
}
