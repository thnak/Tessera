// SPDX-License-Identifier: MIT
//
// Manual measurement tool (not part of the CTest run): the steady-state cost of an animated 4K
// vector scene on the GPU path rasterizer (ADR-071, issue #685), against the ThorVG
// raster-and-upload numbers cpu_to_gpu_4k_measure prints.
//
// A document of filled blobs and round-cap strokes is rasterized into a 3840x2160 texture every
// frame at a slightly different scale (an animated zoom inside one scale bucket). Per frame it
// reports the CPU time of enqueue + flush (what the UI thread pays; nothing waits on the GPU), the
// bytes the CPU wrote to device buffers, and, separately, the time until the GPU finished.
//
//   gpu_path_animated_measure [--frames N] [--blobs N]

#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

namespace rhi = tessera::rendering::rhi;
namespace path = tessera::rendering::path;
namespace vk = tessera::rendering::vulkan;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double micros(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

[[nodiscard]] path::Path blob(float cx, float cy, float r) {
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

[[nodiscard]] std::vector<path::PathShape> scene(int blobs) {
    std::vector<path::PathShape> shapes;
    for (int i = 0; i < blobs; ++i) {
        const float cx = 80.0f + static_cast<float>(i % 20) * 90.0f;
        const float cy = 80.0f + static_cast<float>(i / 20) * 90.0f;
        path::PathShape fill;
        fill.path = blob(cx, cy, 36.0f);
        fill.paint.fill = path::Rgba8{static_cast<std::uint8_t>(40 + i * 7), 120, 200, 255};
        shapes.push_back(std::move(fill));
        path::PathShape line;
        line.path = blob(cx, cy, 20.0f);
        path::StrokeStyle stroke;
        stroke.color = {250, 250, 250, 255};
        stroke.width = 4.0f;
        stroke.cap = path::LineCap::round;
        stroke.join = path::LineJoin::round;
        line.paint.stroke = stroke;
        shapes.push_back(std::move(line));
    }
    return shapes;
}

struct Summary {
    double min = 0;
    double median = 0;
    double max = 0;
};

[[nodiscard]] Summary summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values.front(), values[values.size() / 2], values.back()};
}

} // namespace

int main(int argc, char** argv) {
    int frames = 120;
    int blobs = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            frames = std::atoi(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--blobs") == 0) {
            blobs = std::atoi(argv[i + 1]);
        }
    }
    try {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Vulkan;
        auto device = tessera::rendering::vulkan::create_vulkan_device(desc);
        if (!path::GpuPathRasterizer::supported(*device)) {
            std::puts("gpu_path_animated_measure: no compute or buffer-to-texture copy");
            return 0;
        }
        constexpr std::uint32_t kWidth = 3840;
        constexpr std::uint32_t kHeight = 2160;
        // A 4K output buffer is 33 MB; the default 16 MiB pool bound would destroy it after every
        // flush, so the steady state needs a larger bound (reported below).
        path::GpuPathRasterizerConfig config;
        config.max_pooled_bytes = 256ULL * 1024ULL * 1024ULL;
        path::GpuPathRasterizer rasterizer(*device, vk::path_raster_pipeline_descs(), config);
        (void)rasterizer.warm(2);

        const auto shapes = scene(blobs);
        const auto document = path::make_gpu_document(shapes, 2.0f);
        rhi::TextureDesc texture_desc;
        texture_desc.extent = {kWidth, kHeight};
        auto texture = device->create_texture(texture_desc);

        std::vector<double> cpu_us;
        std::vector<double> gpu_us;
        std::vector<double> bytes;
        std::uint64_t last_bytes = 0;
        for (int frame = 0; frame < frames + 4; ++frame) {
            const float scale = 1.90f + 0.10f * static_cast<float>(frame % 16) / 15.0f;
            const auto t0 = Clock::now();
            const auto queued = rasterizer.enqueue(document, scale, *texture, {kWidth, kHeight});
            const auto flushed = rasterizer.flush();
            const auto t1 = Clock::now();
            if (!queued.has_value() || !flushed.has_value()) {
                std::puts("gpu_path_animated_measure: enqueue or flush refused");
                return 1;
            }
            device->wait_idle();
            const auto t2 = Clock::now();
            rasterizer.collect();
            const std::uint64_t now_bytes = rasterizer.stats().uploaded_bytes;
            if (frame >= 4) { // the first frames upload the document and create the buffers
                cpu_us.push_back(micros(t0, t1));
                gpu_us.push_back(micros(t1, t2));
                bytes.push_back(static_cast<double>(now_bytes - last_bytes));
            }
            last_bytes = now_bytes;
        }
        const auto stats = rasterizer.stats();
        const Summary cpu = summarize(cpu_us);
        const Summary gpu = summarize(gpu_us);
        const Summary up = summarize(bytes);
        std::printf("scene: %zu paths, document %zu bytes, target %ux%u\n", shapes.size(),
                    document->byte_size(), kWidth, kHeight);
        std::printf("CPU enqueue+flush   min %8.1f  median %8.1f  max %8.1f us\n", cpu.min,
                    cpu.median, cpu.max);
        std::printf("GPU until complete  min %8.1f  median %8.1f  max %8.1f us\n", gpu.min,
                    gpu.median, gpu.max);
        std::printf("bytes written/frame min %8.0f  median %8.0f  max %8.0f\n", up.min, up.median,
                    up.max);
        std::printf("arena: %llu upload(s), %llu hit(s); buffers created %llu, reused %llu\n",
                    static_cast<unsigned long long>(stats.resident_uploads),
                    static_cast<unsigned long long>(stats.resident_hits),
                    static_cast<unsigned long long>(stats.buffers_created),
                    static_cast<unsigned long long>(stats.buffers_reused));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gpu_path_animated_measure: %s\n", error.what());
        return 2;
    }
    return 0;
}
