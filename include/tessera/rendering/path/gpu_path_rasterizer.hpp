// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace tessera::rendering::path {

// The four compute passes of the GPU path rasterizer (ADR-071), as the backend describes them. The
// path library knows no backend: the Vulkan lib builds these with `path_raster_pipeline_desc`.
struct GpuPathPipelineDescs {
    rhi::ComputePipelineDesc clear;
    rhi::ComputePipelineDesc stroke;
    rhi::ComputePipelineDesc bin;
    rhi::ComputePipelineDesc fine;
};

struct GpuPathRasterizerConfig {
    // Rasters one flush() can carry. A compute pipeline owns its one descriptor set, so two jobs of
    // a submission cannot share a pipeline object (RHI 1.16): each job takes a slot of four from a
    // pool, and a slot returns to it when its submission completes. Slots are created on demand, so
    // the pool is as large as the most jobs ever in flight at once.
    std::uint32_t max_jobs_per_batch = 8;
    // Bound on the idle buffers the pool keeps between batches (capacity bytes); past it a returned
    // buffer is destroyed. 16 MiB holds a few dozen 256 px rasters or two 1024 px ones.
    std::uint64_t max_pooled_bytes = 16ULL * 1024ULL * 1024ULL;
};

enum class GpuRasterError : std::uint8_t {
    unsupported = 0, // the device has no compute or no buffer-to-texture copy
    batch_full,      // `max_jobs_per_batch` jobs are pending: flush(), then enqueue again
    too_large,       // a size does not fit the shader's 32-bit indices or the dispatch limit
    device_failure,  // the device refused a buffer, pipeline or submission
};

// Rasterizes shapes on the GPU into textures, many per submission.
//
// enqueue() encodes on the CPU and queues the work; nothing reaches the GPU until flush(), which
// records every queued raster (four passes and the copy) into one command list and submits it
// without waiting. The submission is ordered before any draw submitted after it on the same queue,
// so the target texture is ready when the frame that samples it runs. `collect()` frees a flushed
// batch's buffers once the GPU is done with it; call it once per frame. Not thread-safe.
class GpuPathRasterizer {
public:
    struct Stats {
        std::uint64_t jobs = 0;            // rasters submitted
        std::uint64_t flushes = 0;         // submissions
        std::uint32_t pending = 0;         // queued, not yet flushed
        std::uint32_t in_flight = 0;       // flushed, not yet collected
        std::uint32_t pipeline_slots = 0;  // job slots created so far
        std::uint64_t buffers_created = 0; // device buffers allocated
        std::uint64_t buffers_reused = 0;  // buffer requests served by the pool
        std::uint64_t pooled_bytes = 0;    // idle buffer capacity held
    };

    GpuPathRasterizer(rhi::Device& device, GpuPathPipelineDescs descs,
                      GpuPathRasterizerConfig config = {});
    ~GpuPathRasterizer();

    GpuPathRasterizer(const GpuPathRasterizer&) = delete;
    GpuPathRasterizer& operator=(const GpuPathRasterizer&) = delete;

    // True when `device` can run this rasterizer: compute plus the buffer-to-texture copy.
    [[nodiscard]] static bool supported(const rhi::Device& device);

    // Creates job slots up front so the first batch does not pay for them: building the four
    // pipelines of a slot took about 2 ms on the RX 5300M, so eight jobs in a first frame is a
    // 16 ms hitch. Call it at startup, off the frame path. Never shrinks or rebuilds existing
    // slots.
    [[nodiscard]] std::expected<void, GpuRasterError> warm(std::uint32_t slots);

    // Queues `shapes` (already scaled to the target) to be rasterized into `target`, a texture of
    // `extent` this device created. `target` must stay alive until the batch is collected.
    [[nodiscard]] std::expected<void, GpuRasterError>
    enqueue(std::span<const PathShape> shapes, rhi::Texture& target, rhi::Extent2D extent);

    // Submits every queued raster in one command list. A no-op without pending work.
    [[nodiscard]] std::expected<void, GpuRasterError> flush();

    // Releases the buffers of flushed batches the GPU has finished. Never blocks.
    void collect();

    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::rendering::path
