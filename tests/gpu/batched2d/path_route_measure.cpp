// SPDX-License-Identifier: MIT
//
// Manual measurement tool (not part of the CTest run): where the CPU and the GPU route of
// PathTextureCache cross over (ADR-071). For each SVG fixture and each size, a stream of distinct
// icons is requested through the cache once with ThorVG only and once with the GPU rasterizer
// forced on (`gpu_when_possible`, which skips the size line), and the time is split in two: the
// main thread's (acquire() plus flush_gpu(), the cost a frame pays) and the end-to-end one (until
// the device is idle, the latency before the pixels exist). The route policy's `min_pixels` is
// chosen from the first column.

#include "path_texture_draw_support.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;
namespace vk = tessera::rendering::vulkan;

using Clock = std::chrono::steady_clock;

constexpr int kIcons = 48;
constexpr int kRepetitions = 10;
constexpr int kWarmup = 3;

[[nodiscard]] double ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

struct Times {
    double main_ms = 1e9;  // acquire() loop plus flush_gpu(), best repetition
    double total_ms = 1e9; // until the device is idle
    std::uint64_t gpu = 0;
    std::uint64_t fallbacks = 0;
};

// One stream of kIcons distinct icons through `cache`, best of kRepetitions.
[[nodiscard]] Times stream(rhi::Device& device, path::PathTextureCache& cache,
                           const path::SvgDocument& document, std::uint32_t scale_milli) {
    Times best;
    for (int rep = 0; rep < kRepetitions + kWarmup; ++rep) {
        cache.invalidate_all();
        const auto begin = Clock::now();
        for (int icon = 0; icon < kIcons; ++icon) {
            const path::SvgTextureKey key = path::make_svg_texture_key(
                1000U + static_cast<std::uint64_t>(icon), 1, document, scale_milli);
            if (!path::acquire_svg_texture(cache, key, document).has_value()) {
                std::cerr << "path_route_measure: acquire failed\n";
                return best;
            }
        }
        (void)cache.flush_gpu();
        const auto submitted = Clock::now();
        device.wait_idle();
        const auto idle = Clock::now();
        cache.end_frame();
        if (rep >= kWarmup) {
            best.main_ms = std::min(best.main_ms, ms_between(begin, submitted));
            best.total_ms = std::min(best.total_ms, ms_between(begin, idle));
        }
    }
    best.gpu = cache.stats().gpu_rasterizations;
    best.fallbacks = cache.stats().gpu_fallbacks;
    return best;
}

void measure_fixture(rhi::Device& device, const std::string& name) {
    const auto bytes = tessera::test::path_draw::read_svg_fixture(name);
    auto cpu_rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (bytes.empty() || cpu_rasterizer == nullptr) {
        return;
    }
    path::SvgDocumentCache documents;
    const auto key = path::make_svg_document_key(std::span<const std::uint8_t>(bytes));
    const auto document = documents.acquire(key, std::span<const std::uint8_t>(bytes));
    if (!document.has_value() || document.value()->width == 0) {
        return;
    }
    const path::SvgDocument& decoded = *document.value();

    path::PathTextureCacheConfig config;
    config.max_entries = 128;
    config.max_bytes = 256ULL * 1024ULL * 1024ULL;

    path::GpuPathRasterizer gpu(device, vk::path_raster_pipeline_descs());
    (void)gpu.warm(2);

    for (const std::uint32_t size : {32U, 48U, 64U, 96U, 128U, 192U, 256U}) {
        const std::uint32_t scale_milli = std::max(1U, size * 1000U / decoded.width);
        path::PathTextureCache cpu_cache(device, *cpu_rasterizer, config);
        const Times cpu = stream(device, cpu_cache, decoded, scale_milli);

        path::PathTextureCache gpu_cache(device, *cpu_rasterizer, config);
        path::RasterRouteConfig forced;
        forced.mode = path::RasterRouteMode::gpu_when_possible;
        gpu_cache.set_gpu_rasterizer(&gpu, forced);
        const Times on_gpu = stream(device, gpu_cache, decoded, scale_milli);

        const double per = 1.0 / kIcons;
        std::cout << std::fixed << std::setprecision(4) << "ROUTE " << name << " " << size
                  << "px: cpu main " << cpu.main_ms * per << " total " << cpu.total_ms * per
                  << " | gpu main " << on_gpu.main_ms * per << " total " << on_gpu.total_ms * per
                  << " (gpu rasters " << on_gpu.gpu << ", fallbacks " << on_gpu.fallbacks << ")"
                  << " ms per icon\n";
    }
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    try {
        const auto device = vk::create_vulkan_device(device_desc);
        if (!path::GpuPathRasterizer::supported(*device)) {
            std::cerr << "path_route_measure: no compute or buffer-to-texture copy\n";
            return 1;
        }
        std::cout << "device: " << "see Vulkan device line of the loader\n";
        for (const char* name :
             {"icon_shapes.svg", "stroke_path.svg", "group_transform.svg", "viewbox_scale.svg"}) {
            measure_fixture(*device, name);
        }
    } catch (const std::exception& error) {
        std::cerr << "path_route_measure: skipped (" << error.what() << ")\n";
        return 1;
    }
    return 0;
}
