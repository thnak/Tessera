// SPDX-License-Identifier: MIT

// Emscripten-only: the animated 4K frame of tests/gpu/batched2d/gpu_path_animated_measure.cpp on
// the WebGPU backend (issue #685, ADR-071), through the production GpuPathRasterizer (document
// residency, the arena, the buffer-to-texture handoff) and the WebGPU path-raster programs. Per
// frame it reports the CPU time of enqueue + flush, the time from the end of the flush until the
// queue reported the work done (readback is not part of it; the target is a texture a draw would
// sample), and the bytes the CPU wrote to device buffers.

#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/webgpu/path_raster_pipeline.hpp"
#include "webgpu_internal.hpp"
#include "webgpu_path_raster_smoke.hpp"
#include "webgpu_path_smoke_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace tessera::path_smoke {
namespace {

namespace path = tessera::rendering::path;
using Clock = std::chrono::steady_clock;

double micros(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

path::Path blob(float cx, float cy, float r) {
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

// The sparse scene of gpu_path_animated_measure: filled blobs, each with a round-cap stroked ring.
std::vector<path::PathShape> sparse_scene(int blobs) {
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

// Thirteen overlapping full-frame fills: every pixel is covered by many paths.
std::vector<path::PathShape> dense_scene() {
    std::vector<path::PathShape> shapes;
    for (int i = 0; i < 13; ++i) {
        path::PathShape fill;
        fill.path = blob(960.0f + static_cast<float>(i - 6) * 20.0f,
                         540.0f + static_cast<float>(i % 5 - 2) * 15.0f, 1050.0f - 40.0f * i);
        fill.paint.fill = path::Rgba8{static_cast<std::uint8_t>(30 + i * 15),
                                      static_cast<std::uint8_t>(200 - i * 10), 120, 255};
        shapes.push_back(std::move(fill));
    }
    return shapes;
}

struct Summary {
    double min = 0;
    double median = 0;
    double max = 0;
};

Summary summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values.front(), values[values.size() / 2], values.back()};
}

// Resolves when the queue has finished everything submitted so far.
struct QueueDone {
    bool done = false;
};

void on_work_done(WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata, void*) {
    static_cast<QueueDone*>(userdata)->done = true;
}

bool wait_queue(WGPUQueue queue) {
    QueueDone state;
    WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = on_work_done;
    info.userdata1 = &state;
    wgpuQueueOnSubmittedWorkDone(queue, info);
    return wait_for([&] { return state.done; });
}

} // namespace

int run_measure(rhi::Device& device, void* wgpu_queue, const MeasureOptions& options) {
    if (!path::GpuPathRasterizer::supported(device)) {
        std::puts("webgpu_path_raster: the device has no compute or buffer-to-texture copy");
        return 1;
    }
    constexpr std::uint32_t kWidth = 3840;
    constexpr std::uint32_t kHeight = 2160;
    path::GpuPathRasterizerConfig config;
    // A 4K output buffer is 33 MB; the burst below keeps 16 flights' buffers alive, so the pool
    // must hold them for the rounds after the first to reuse rather than reallocate (the device
    // clears every new buffer).
    config.max_pooled_bytes = 3ULL * 1024ULL * 1024ULL * 1024ULL;
    path::GpuPathRasterizer rasterizer(device, webgpu::path_raster_pipeline_descs(), config);
    (void)rasterizer.warm(2);

    const auto shapes = options.dense ? dense_scene() : sparse_scene(options.blobs);
    const auto document = path::make_gpu_document(shapes, 2.0f);
    rhi::TextureDesc texture_desc;
    texture_desc.extent = {kWidth, kHeight};
    auto texture = device.create_texture(texture_desc);
    const auto queue = static_cast<WGPUQueue>(wgpu_queue);

    // The latency of an empty queue: the floor of every "until complete" number below (the host's
    // callback polling and the driver's submission, nothing the rasterizer does).
    std::vector<double> floor_us;
    for (int i = 0; i < 30; ++i) {
        const auto f0 = Clock::now();
        (void)wait_queue(queue);
        floor_us.push_back(micros(f0, Clock::now()));
    }

    std::vector<double> cpu_us;
    std::vector<double> gpu_us;
    std::vector<double> bytes;
    std::uint64_t last_bytes = 0;
    for (int frame = 0; frame < options.frames + 4; ++frame) {
        const float scale = 1.90f + 0.10f * static_cast<float>(frame % 16) / 15.0f;
        const auto t0 = Clock::now();
        const auto queued = rasterizer.enqueue(document, scale, *texture, {kWidth, kHeight});
        const auto flushed = rasterizer.flush();
        const auto t1 = Clock::now();
        if (!queued.has_value() || !flushed.has_value()) {
            // GpuRasterError: 0 unsupported, 1 batch_full, 2 too_large, 3 too_dense, 4
            // device_failure.
            std::printf(
                "webgpu_path_raster: enqueue refused (%d) or flush refused (%d) at frame %d\n",
                queued.has_value() ? -1 : static_cast<int>(queued.error()),
                flushed.has_value() ? -1 : static_cast<int>(flushed.error()), frame);
            return 1;
        }
        if (!wait_queue(queue)) {
            std::puts("webgpu_path_raster: the queue never reported the work done");
            return 1;
        }
        const auto t2 = Clock::now();
        // The submission's own completion callback may trail the queue-wide one by a tick.
        (void)wait_for([&] {
            rasterizer.collect();
            return rasterizer.stats().in_flight == 0;
        });
        const std::uint64_t now_bytes = rasterizer.stats().uploaded_bytes;
        if (frame >= 4) { // the first frames upload the document and create the buffers
            cpu_us.push_back(micros(t0, t1));
            gpu_us.push_back(micros(t1, t2));
            bytes.push_back(static_cast<double>(now_bytes - last_bytes));
        }
        last_bytes = now_bytes;
    }
    // Several frames submitted back to back and waited for once: the latency above is paid once, so
    // the per-frame figure approaches the GPU's own time (bounded by 16 in flight: each flight
    // holds its own 4K output and work buffers).
    constexpr int kBurst = 16;
    std::vector<double> burst_us;
    for (int round = 0; round < 10; ++round) {
        const auto b0 = Clock::now();
        for (int i = 0; i < kBurst; ++i) {
            const float scale = 1.90f + 0.10f * static_cast<float>(i) / 15.0f;
            if (!rasterizer.enqueue(document, scale, *texture, {kWidth, kHeight}).has_value() ||
                !rasterizer.flush().has_value()) {
                std::puts("webgpu_path_raster: burst enqueue or flush refused");
                return 1;
            }
        }
        if (!wait_queue(queue)) {
            return 1;
        }
        burst_us.push_back(micros(b0, Clock::now()) / kBurst);
        (void)wait_for([&] {
            rasterizer.collect();
            return rasterizer.stats().in_flight == 0;
        });
    }
    const auto stats = rasterizer.stats();
    const Summary cpu = summarize(cpu_us);
    const Summary gpu = summarize(gpu_us);
    const Summary up = summarize(bytes);
    std::printf("scene: %zu paths, document %zu bytes, target %ux%u, %d frames\n", shapes.size(),
                document->byte_size(), kWidth, kHeight, options.frames);
    std::printf("CPU enqueue+flush   min %8.1f  median %8.1f  max %8.1f us\n", cpu.min, cpu.median,
                cpu.max);
    std::printf("GPU until complete  min %8.1f  median %8.1f  max %8.1f us\n", gpu.min, gpu.median,
                gpu.max);
    const Summary floor = summarize(floor_us);
    const Summary burst = summarize(burst_us);
    std::printf(
        "empty-queue wait    min %8.1f  median %8.1f  max %8.1f us (the floor of the line above)\n",
        floor.min, floor.median, floor.max);
    std::printf("burst of %d / frame  min %8.1f  median %8.1f  max %8.1f us (enqueue+flush+GPU, "
                "one wait)\n",
                kBurst, burst.min, burst.median, burst.max);
    std::printf("bytes written/frame min %8.0f  median %8.0f  max %8.0f\n", up.min, up.median,
                up.max);
    std::printf("arena: %llu upload(s), %llu hit(s); buffers created %llu, reused %llu\n",
                static_cast<unsigned long long>(stats.resident_uploads),
                static_cast<unsigned long long>(stats.resident_hits),
                static_cast<unsigned long long>(stats.buffers_created),
                static_cast<unsigned long long>(stats.buffers_reused));
    return 0;
}

} // namespace tessera::path_smoke
