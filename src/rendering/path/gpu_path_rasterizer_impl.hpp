// SPDX-License-Identifier: MIT
//
// Internals shared by the GPU path rasterizer's translation units (gpu_path_rasterizer.cpp and
// gpu_path_rasterizer_pack.cpp); not a public header.

#pragma once

#include "gpu_path_arena.hpp"
#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/gpu_path_scene.hpp"
#include "tessera/rendering/rhi/texture_region.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace tessera::rendering::path {

constexpr std::uint32_t kGroupSize = 256;
constexpr std::uint32_t kTile = 16;
constexpr std::uint32_t kMaxGroups = 65535; // the RHI's dispatch limit on every target device
constexpr std::size_t kMinBufferBytes = 4096;
constexpr std::uint64_t kIndexLimit = std::numeric_limits<std::uint32_t>::max();

// The shared buffers of a batch, by shader binding.
constexpr std::size_t kSegs = 0;
constexpr std::size_t kPaths = 1;
constexpr std::size_t kWork = 2;
constexpr std::size_t kOutput = 3;
constexpr std::size_t kStrokes = 4;
constexpr std::size_t kDashes = 5;
constexpr std::size_t kJobs = 6;
constexpr std::size_t kBufferCount = 7;

[[nodiscard]] constexpr bool is_read_write(std::size_t binding) {
    return binding == kWork || binding == kOutput;
}

[[nodiscard]] inline std::uint32_t groups(std::uint32_t count, std::uint32_t size) {
    return (count + size - 1U) / size;
}

// Which of the buffers a pass reads or writes (bit n = binding n), taken from
// shaders/path_raster.comp. Every pass reads the jobs table. A binding a pass does not declare is
// not in its SPIR-V interface, so leaving it unwritten is valid. If a pass ever starts using
// another buffer, this table and the shader change together, and the Vulkan pixel tests (validation
// on) fail otherwise.
constexpr std::uint32_t kJobsBit = 1U << kJobs;
constexpr std::uint32_t kClearBindings = (1U << kWork) | kJobsBit;
constexpr std::uint32_t kStrokeBindings =
    (1U << kWork) | (1U << kStrokes) | (1U << kDashes) | kJobsBit;
constexpr std::uint32_t kBinBindings = (1U << kSegs) | (1U << kPaths) | (1U << kWork) | kJobsBit;
constexpr std::uint32_t kFineBindings =
    (1U << kSegs) | (1U << kPaths) | (1U << kWork) | (1U << kOutput) | kJobsBit;

struct GpuPathRasterizer::Impl {
    // The four pipelines one in-flight batch owns.
    struct Slot {
        std::unique_ptr<rhi::ComputePipeline> clear;
        std::unique_ptr<rhi::ComputePipeline> stroke;
        std::unique_ptr<rhi::ComputePipeline> bin;
        std::unique_ptr<rhi::ComputePipeline> fine;
    };

    // One queued raster: its encoded words and sizes, held until flush() packs the batch.
    struct Job {
        std::shared_ptr<const EncodedGpuScene> scene; // shared when it comes from a document
        std::vector<std::uint32_t> paths;             // this raster's records (kGpuPathWords each)
        PathRasterLayout layout;
        rhi::Texture* target = nullptr;
        rhi::Extent2D extent;
        std::size_t pitch = 0;
        bool resident = false; // geometry lives in the arena at `slice`
        GpuPathArena::Slice slice;
    };

    [[nodiscard]] std::expected<void, GpuRasterError>
    queue(std::shared_ptr<const EncodedGpuScene> scene, std::vector<std::uint32_t> paths,
          float max_scale, rhi::Texture& target, rhi::Extent2D extent, std::uint64_t max_row_nodes,
          bool resident_ok);

    struct Flight {
        std::unique_ptr<rhi::CommandList> list;
        std::unique_ptr<Slot> slot;
        std::array<std::unique_ptr<rhi::Buffer>, kBufferCount> buffers;
        std::uint32_t jobs = 0;
        bool uses_arena = false; // bindings 0, 4 and 5 are the arena's, not this flight's
    };

    Impl(rhi::Device& target_device, GpuPathPipelineDescs pipeline_descs,
         GpuPathRasterizerConfig cache_config)
        : device(target_device),
          descs(pipeline_descs),
          config(cache_config) {
        config.max_jobs_per_batch = std::clamp(config.max_jobs_per_batch, 1U, kMaxGroups);
        if (config.resident_bytes != 0) {
            arena = std::make_unique<GpuPathArena>(device, config.resident_bytes);
        }
    }

    // True while a queued or in-flight raster reads the arena's buffers, so it must not be reset.
    [[nodiscard]] bool arena_busy() const {
        return std::any_of(pending.begin(), pending.end(),
                           [](const Job& job) { return job.resident; }) ||
               std::any_of(in_flight.begin(), in_flight.end(),
                           [](const auto& flight) { return flight->uses_arena; });
    }

    // Makes `job`'s geometry resident, resetting a full arena when nothing reads it.
    void make_resident(Job& job) {
        if (!arena) {
            return;
        }
        auto slice = arena->acquire(job.scene);
        if (!slice && !arena_busy()) {
            arena->reset();
            slice = arena->acquire(job.scene);
        }
        if (slice) {
            job.resident = true;
            job.slice = *slice;
        }
    }

    [[nodiscard]] std::unique_ptr<Slot> take_slot() {
        if (!free_slots.empty()) {
            auto slot = std::move(free_slots.back());
            free_slots.pop_back();
            return slot;
        }
        auto slot = std::make_unique<Slot>();
        slot->clear = device.create_compute_pipeline(descs.clear);
        slot->stroke = device.create_compute_pipeline(descs.stroke);
        slot->bin = device.create_compute_pipeline(descs.bin);
        slot->fine = device.create_compute_pipeline(descs.fine);
        if (!slot->clear || !slot->stroke || !slot->bin || !slot->fine) {
            return nullptr;
        }
        ++slots_created;
        return slot;
    }

    // Buffers are pooled by capacity (a power of two, at least 4 KiB): a batch takes the smallest
    // free one that fits and the pool takes it back when the batch's flight completes, so a steady
    // stream of icons allocates nothing. A reused buffer holds stale bytes past what a job writes;
    // that is safe because every pass reads only what its job record bounds, the clear pass resets
    // the work region's counter and heads, and the fine pass writes every output pixel.
    [[nodiscard]] std::unique_ptr<rhi::Buffer> acquire_buffer(std::size_t bytes, bool read_write) {
        auto& pool = read_write ? free_read_write : free_read_only;
        std::size_t best = pool.size();
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (pool[index]->size() >= bytes &&
                (best == pool.size() || pool[index]->size() < pool[best]->size())) {
                best = index;
            }
        }
        if (best != pool.size()) {
            auto buffer = std::move(pool[best]);
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(best));
            pooled_bytes -= buffer->size();
            ++buffers_reused;
            return buffer;
        }
        rhi::BufferDesc desc{std::bit_ceil(std::max(bytes, kMinBufferBytes)),
                             rhi::BufferUsage::Storage};
        desc.gpu_read_write = read_write;
        ++buffers_created;
        return device.create_buffer(desc);
    }

    void release_buffer(std::unique_ptr<rhi::Buffer> buffer, bool read_write) {
        if (!buffer || pooled_bytes + buffer->size() > config.max_pooled_bytes) {
            return; // over the pool bound: the buffer is destroyed
        }
        pooled_bytes += buffer->size();
        (read_write ? free_read_write : free_read_only).push_back(std::move(buffer));
    }

    // Word totals of the pending jobs, which become the shared buffers' sizes and the next job's
    // bases. They must stay inside the shader's 32-bit indices.
    struct Totals {
        std::uint64_t segs = 0;
        std::uint64_t paths = 0;
        std::uint64_t strokes = 0;
        std::uint64_t dashes = 0;
        std::uint64_t work = 0;
        std::uint64_t output = 0;
    };

    [[nodiscard]] static Totals totals_of(const std::vector<Job>& jobs) {
        Totals total;
        for (const Job& job : jobs) {
            total.segs += job.scene->segs.size();
            total.paths += job.paths.size();
            total.strokes += job.scene->strokes.size();
            total.dashes += job.scene->dashes.size();
            total.work += job.layout.work_words;
            total.output += job.layout.output_words;
        }
        return total;
    }

    [[nodiscard]] bool fits(const Job& job) const {
        const Totals total = totals_of(pending);
        return total.segs + job.scene->segs.size() <= kIndexLimit &&
               total.paths + job.paths.size() <= kIndexLimit &&
               total.strokes + job.scene->strokes.size() <= kIndexLimit &&
               total.dashes + job.scene->dashes.size() <= kIndexLimit &&
               total.work + job.layout.work_words <= kIndexLimit &&
               total.output + job.layout.output_words <= kIndexLimit;
    }

    // Packs the pending jobs into the shared buffers and records the four passes once for all of
    // them, then one copy per job into its texture. Returns the flight, or nothing on a device
    // error.
    [[nodiscard]] std::unique_ptr<Flight> pack_and_record();

    // Returns a flight's slot and buffers to their pools.
    void retire(Flight& flight) {
        if (flight.slot) {
            free_slots.push_back(std::move(flight.slot));
        }
        for (std::size_t binding = 0; binding < kBufferCount; ++binding) {
            release_buffer(std::move(flight.buffers[binding]), is_read_write(binding));
        }
    }

    rhi::Device& device;
    GpuPathPipelineDescs descs;
    GpuPathRasterizerConfig config;
    std::vector<std::unique_ptr<Slot>> free_slots;
    std::uint32_t slots_created = 0;
    std::vector<std::unique_ptr<rhi::Buffer>> free_read_only;
    std::vector<std::unique_ptr<rhi::Buffer>> free_read_write;
    std::uint64_t pooled_bytes = 0;
    std::uint64_t buffers_created = 0;
    std::uint64_t buffers_reused = 0;
    std::unique_ptr<GpuPathArena> arena; // null when resident_bytes is 0
    std::uint64_t uploaded_bytes = 0;
    std::vector<Job> pending;
    std::vector<std::unique_ptr<Flight>> in_flight;
    std::uint64_t jobs = 0;
    std::uint64_t flushes = 0;
};

} // namespace tessera::rendering::path
