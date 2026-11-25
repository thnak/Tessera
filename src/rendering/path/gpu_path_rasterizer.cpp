// SPDX-License-Identifier: MIT

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
namespace {

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

[[nodiscard]] std::uint32_t groups(std::uint32_t count, std::uint32_t size) {
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

} // namespace

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
        EncodedGpuScene encoded;
        PathRasterLayout layout;
        rhi::Texture* target = nullptr;
        rhi::Extent2D extent;
        std::size_t pitch = 0;
    };

    struct Flight {
        std::unique_ptr<rhi::CommandList> list;
        std::unique_ptr<Slot> slot;
        std::array<std::unique_ptr<rhi::Buffer>, kBufferCount> buffers;
        std::uint32_t jobs = 0;
    };

    Impl(rhi::Device& target_device, GpuPathPipelineDescs pipeline_descs,
         GpuPathRasterizerConfig cache_config)
        : device(target_device),
          descs(pipeline_descs),
          config(cache_config) {
        config.max_jobs_per_batch = std::clamp(config.max_jobs_per_batch, 1U, kMaxGroups);
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
            total.segs += job.encoded.segs.size();
            total.paths += job.encoded.paths.size();
            total.strokes += job.encoded.strokes.size();
            total.dashes += job.encoded.dashes.size();
            total.work += job.layout.work_words;
            total.output += job.layout.output_words;
        }
        return total;
    }

    [[nodiscard]] bool fits(const Job& job) const {
        const Totals total = totals_of(pending);
        return total.segs + job.encoded.segs.size() <= kIndexLimit &&
               total.paths + job.encoded.paths.size() <= kIndexLimit &&
               total.strokes + job.encoded.strokes.size() <= kIndexLimit &&
               total.dashes + job.encoded.dashes.size() <= kIndexLimit &&
               total.work + job.layout.work_words <= kIndexLimit &&
               total.output + job.layout.output_words <= kIndexLimit;
    }

    // Packs the pending jobs into the shared buffers and records the four passes once for all of
    // them, then one copy per job into its texture. Returns the flight, or nothing on a device
    // error.
    [[nodiscard]] std::unique_ptr<Flight> pack_and_record() {
        const std::uint32_t count = static_cast<std::uint32_t>(pending.size());
        const Totals total = totals_of(pending);
        const auto words = [](std::uint64_t value, std::size_t minimum) {
            return std::max<std::size_t>(static_cast<std::size_t>(value), minimum) *
                   sizeof(std::uint32_t);
        };

        auto flight = std::make_unique<Flight>();
        flight->jobs = count;
        flight->buffers[kSegs] = acquire_buffer(words(total.segs, kGpuSegWords), false);
        flight->buffers[kPaths] = acquire_buffer(words(total.paths, kGpuPathWords), false);
        flight->buffers[kWork] = acquire_buffer(words(total.work, 1), true);
        flight->buffers[kOutput] = acquire_buffer(words(total.output, 1), true);
        flight->buffers[kStrokes] = acquire_buffer(words(total.strokes, kGpuStrokeWords), false);
        flight->buffers[kDashes] = acquire_buffer(words(total.dashes, kGpuDashWords), false);
        flight->buffers[kJobs] =
            acquire_buffer(static_cast<std::size_t>(count) * sizeof(PathRasterJob), false);

        std::vector<PathRasterJob> records(count);
        Totals base;
        std::uint32_t max_clear = 0;
        std::uint32_t max_stroke = 0;
        std::uint32_t max_seg = 0;
        std::uint32_t max_width = 0;
        std::uint32_t max_height = 0;
        const auto put = [&](std::size_t binding, const std::vector<std::uint32_t>& data,
                             std::uint64_t at) {
            if (!data.empty()) {
                flight->buffers[binding]->write(data.data(), data.size() * sizeof(std::uint32_t),
                                                static_cast<std::size_t>(at) *
                                                    sizeof(std::uint32_t));
            }
        };
        for (std::uint32_t index = 0; index < count; ++index) {
            const Job& job = pending[index];
            PathRasterJob& record = records[index];
            record.push = job.layout.push;
            record.seg_base = static_cast<std::uint32_t>(base.segs);
            record.path_base = static_cast<std::uint32_t>(base.paths);
            record.work_base = static_cast<std::uint32_t>(base.work);
            record.out_base = static_cast<std::uint32_t>(base.output);
            record.stroke_base = static_cast<std::uint32_t>(base.strokes);
            record.dash_base = static_cast<std::uint32_t>(base.dashes);
            put(kSegs, job.encoded.segs, base.segs);
            put(kPaths, job.encoded.paths, base.paths);
            put(kStrokes, job.encoded.strokes, base.strokes);
            put(kDashes, job.encoded.dashes, base.dashes);
            base.segs += job.encoded.segs.size();
            base.paths += job.encoded.paths.size();
            base.strokes += job.encoded.strokes.size();
            base.dashes += job.encoded.dashes.size();
            base.work += job.layout.work_words;
            base.output += job.layout.output_words;
            max_clear = std::max(max_clear, job.layout.push.nodes_base);
            max_stroke = std::max(max_stroke, job.encoded.stroke_count);
            max_seg = std::max(max_seg, job.encoded.seg_count);
            max_width = std::max(max_width, job.extent.width);
            max_height = std::max(max_height, job.extent.height);
        }
        flight->buffers[kJobs]->write(records.data(), records.size() * sizeof(PathRasterJob));

        flight->slot = take_slot();
        if (!flight->slot) {
            retire(*flight);
            return nullptr;
        }
        flight->list = device.create_command_list();
        rhi::CommandList& list = *flight->list;
        const std::uint32_t job_base = 0;
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline, std::uint32_t used) {
            list.bind_compute_pipeline(pipeline);
            for (std::uint32_t binding = 0; binding < kBufferCount; ++binding) {
                if ((used & (1U << binding)) != 0U) {
                    list.bind_compute_buffer(binding, *flight->buffers[binding]);
                }
            }
            list.compute_push_constants(&job_base, sizeof(job_base));
        };
        bind_all(*flight->slot->clear, kClearBindings);
        list.dispatch(groups(max_clear, kGroupSize), count, 1);
        list.compute_barrier();
        if (max_stroke > 0) {
            bind_all(*flight->slot->stroke, kStrokeBindings);
            list.dispatch(groups(max_stroke, kGroupSize), count, 1);
            list.compute_barrier();
        }
        bind_all(*flight->slot->bin, kBinBindings);
        list.dispatch(groups(std::max(max_seg, 1U), kGroupSize), count, 1);
        list.compute_barrier();
        bind_all(*flight->slot->fine, kFineBindings);
        list.dispatch(groups(max_width, kTile), groups(max_height, kTile), count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const Job& job = pending[index];
            rhi::BufferTextureCopy copy;
            copy.region = {0, 0, job.extent.width, job.extent.height};
            copy.buffer_offset =
                static_cast<std::size_t>(records[index].out_base) * sizeof(std::uint32_t);
            copy.buffer_row_pitch = job.pitch;
            list.copy_buffer_to_texture(*flight->buffers[kOutput], *job.target, copy);
        }
        return flight;
    }

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
    std::vector<Job> pending;
    std::vector<std::unique_ptr<Flight>> in_flight;
    std::uint64_t jobs = 0;
    std::uint64_t flushes = 0;
};

GpuPathRasterizer::GpuPathRasterizer(rhi::Device& device, GpuPathPipelineDescs descs,
                                     GpuPathRasterizerConfig config)
    : impl_(std::make_unique<Impl>(device, descs, config)) {}

GpuPathRasterizer::~GpuPathRasterizer() = default;

bool GpuPathRasterizer::supported(const rhi::Device& device) {
    return device.supports_compute() && device.supports_buffer_texture_copy();
}

std::expected<void, GpuRasterError> GpuPathRasterizer::enqueue(std::span<const PathShape> shapes,
                                                               rhi::Texture& target,
                                                               rhi::Extent2D extent,
                                                               std::uint64_t max_row_nodes) {
    if (!supported(impl_->device)) {
        return std::unexpected(GpuRasterError::unsupported);
    }
    if (impl_->pending.size() >= impl_->config.max_jobs_per_batch) {
        return std::unexpected(GpuRasterError::batch_full);
    }
    if (extent.width == 0U || extent.height == 0U) {
        return std::unexpected(GpuRasterError::too_large);
    }

    Impl::Job job;
    job.encoded = encode_gpu_scene(gpu_scene_from_shapes(shapes), extent.width, extent.height);
    job.pitch = rhi::aligned_texture_copy_pitch(extent.width, rhi::TextureFormat::R8G8B8A8Unorm);
    const auto layout = plan_path_raster(job.encoded, extent.width, extent.height,
                                         static_cast<std::uint32_t>(job.pitch / 4U));
    if (!layout || groups(layout->push.nodes_base, kGroupSize) > kMaxGroups ||
        groups(job.encoded.seg_count, kGroupSize) > kMaxGroups ||
        groups(extent.width, kTile) > kMaxGroups || groups(extent.height, kTile) > kMaxGroups) {
        return std::unexpected(GpuRasterError::too_large);
    }
    if (max_row_nodes != 0 && job.encoded.row_nodes > max_row_nodes) {
        return std::unexpected(GpuRasterError::too_dense);
    }
    job.layout = *layout;
    job.target = &target;
    job.extent = extent;
    if (!impl_->fits(job)) {
        return std::unexpected(GpuRasterError::batch_full);
    }
    impl_->pending.push_back(std::move(job));
    return {};
}

void GpuPathRasterizer::discard_pending() {
    impl_->pending.clear();
}

std::expected<void, GpuRasterError> GpuPathRasterizer::warm(std::uint32_t slots) {
    try {
        std::vector<std::unique_ptr<Impl::Slot>> made;
        for (std::uint32_t index = impl_->slots_created; index < slots; ++index) {
            auto slot = impl_->take_slot();
            if (!slot) {
                return std::unexpected(GpuRasterError::device_failure);
            }
            made.push_back(std::move(slot));
        }
        for (auto& slot : made) {
            impl_->free_slots.push_back(std::move(slot));
        }
    } catch (const std::exception&) {
        return std::unexpected(GpuRasterError::device_failure);
    }
    return {};
}

std::expected<void, GpuRasterError> GpuPathRasterizer::flush() {
    if (impl_->pending.empty()) {
        return {};
    }
    try {
        auto flight = impl_->pack_and_record();
        if (!flight) {
            impl_->pending.clear();
            return std::unexpected(GpuRasterError::device_failure);
        }
        flight->list->submit_async();
        impl_->jobs += flight->jobs;
        ++impl_->flushes;
        impl_->pending.clear();
        impl_->in_flight.push_back(std::move(flight));
    } catch (const std::exception&) {
        impl_->pending.clear();
        return std::unexpected(GpuRasterError::device_failure);
    }
    return {};
}

void GpuPathRasterizer::collect() {
    auto& flights = impl_->in_flight;
    for (std::size_t index = 0; index < flights.size();) {
        if (!flights[index]->list->is_complete()) {
            ++index;
            continue;
        }
        impl_->retire(*flights[index]);
        flights.erase(flights.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

GpuPathRasterizer::Stats GpuPathRasterizer::stats() const {
    Stats stats;
    stats.jobs = impl_->jobs;
    stats.flushes = impl_->flushes;
    stats.pending = static_cast<std::uint32_t>(impl_->pending.size());
    stats.in_flight = static_cast<std::uint32_t>(impl_->in_flight.size());
    stats.pipeline_slots = impl_->slots_created;
    stats.buffers_created = impl_->buffers_created;
    stats.buffers_reused = impl_->buffers_reused;
    stats.pooled_bytes = impl_->pooled_bytes;
    return stats;
}

} // namespace tessera::rendering::path
