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
#include <utility>
#include <vector>

namespace tessera::rendering::path {
namespace {

constexpr std::uint32_t kGroupSize = 256;
constexpr std::uint32_t kTile = 16;
constexpr std::uint32_t kMaxGroups = 65535; // the RHI's dispatch limit on every target device

[[nodiscard]] std::uint32_t groups(std::uint32_t count, std::uint32_t size) {
    return (count + size - 1U) / size;
}

constexpr std::size_t kMinBufferBytes = 4096;

// Which of the six storage buffers a pass reads or writes (bit n = binding n), taken from
// shaders/path_raster.comp. Binding only these cuts 24 descriptor updates per raster to 11; a
// binding a pass does not declare is not in its SPIR-V interface, so leaving it unwritten is valid.
// If a pass ever starts using another buffer, this table and the shader change together, and the
// Vulkan pixel test (validation on) fails otherwise.
constexpr std::uint32_t kClearBindings = 1U << 2;
constexpr std::uint32_t kStrokeBindings = (1U << 2) | (1U << 4) | (1U << 5);
constexpr std::uint32_t kBinBindings = (1U << 0) | (1U << 1) | (1U << 2);
constexpr std::uint32_t kFineBindings = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);

} // namespace

struct GpuPathRasterizer::Impl {
    // The four pipelines one job slot owns.
    struct Slot {
        std::unique_ptr<rhi::ComputePipeline> clear;
        std::unique_ptr<rhi::ComputePipeline> stroke;
        std::unique_ptr<rhi::ComputePipeline> bin;
        std::unique_ptr<rhi::ComputePipeline> fine;
    };

    struct Job {
        std::array<std::unique_ptr<rhi::Buffer>, 6>
            buffers; // segs paths work output strokes dashes
        PathRasterLayout layout;
        std::uint32_t stroke_count = 0;
        std::uint32_t seg_count = 0;
        rhi::Texture* target = nullptr;
        rhi::Extent2D extent;
        std::size_t pitch = 0;
    };

    struct Flight {
        std::unique_ptr<rhi::CommandList> list;
        std::vector<Job> jobs;
        // The pipelines this submission binds. Their descriptor sets are updated by every bind, so
        // they go back to the free pool only once the GPU is done with the submission.
        std::vector<std::unique_ptr<Slot>> slots;
    };

    Impl(rhi::Device& target_device, GpuPathPipelineDescs pipeline_descs,
         GpuPathRasterizerConfig cache_config)
        : device(target_device),
          descs(pipeline_descs),
          config(cache_config) {
        config.max_jobs_per_batch = std::max(1U, config.max_jobs_per_batch);
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

    // Buffers are pooled by capacity (a power of two, at least 4 KiB): a raster takes the smallest
    // free one that fits and the pool takes it back when the raster's flight completes, so a steady
    // stream of icons allocates nothing. A reused buffer holds stale bytes past what a job writes;
    // that is safe because every pass reads only what the job's push block bounds, the clear pass
    // resets the work buffer's counter and heads, and the fine pass writes every output pixel.
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

    [[nodiscard]] std::unique_ptr<rhi::Buffer> storage(const std::vector<std::uint32_t>& words,
                                                       std::size_t minimum_words) {
        auto buffer =
            acquire_buffer(std::max(words.size(), minimum_words) * sizeof(std::uint32_t), false);
        if (!words.empty()) {
            buffer->write(words.data(), words.size() * sizeof(std::uint32_t));
        }
        return buffer;
    }

    void record_job(rhi::CommandList& list, const Slot& slot, const Job& job) {
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline, std::uint32_t used) {
            list.bind_compute_pipeline(pipeline);
            for (std::uint32_t binding = 0; binding < job.buffers.size(); ++binding) {
                if ((used & (1U << binding)) != 0U) {
                    list.bind_compute_buffer(binding, *job.buffers[binding]);
                }
            }
            list.compute_push_constants(&job.layout.push, sizeof(job.layout.push));
        };
        bind_all(*slot.clear, kClearBindings);
        list.dispatch(groups(job.layout.push.nodes_base, kGroupSize), 1, 1);
        list.compute_barrier();
        if (job.stroke_count > 0) {
            bind_all(*slot.stroke, kStrokeBindings);
            list.dispatch(groups(job.stroke_count, kGroupSize), 1, 1);
            list.compute_barrier();
        }
        bind_all(*slot.bin, kBinBindings);
        list.dispatch(groups(std::max(job.seg_count, 1U), kGroupSize), 1, 1);
        list.compute_barrier();
        bind_all(*slot.fine, kFineBindings);
        list.dispatch(groups(job.extent.width, kTile), groups(job.extent.height, kTile), 1);
        rhi::BufferTextureCopy copy;
        copy.region = {0, 0, job.extent.width, job.extent.height};
        copy.buffer_row_pitch = job.pitch;
        list.copy_buffer_to_texture(*job.buffers[3], *job.target, copy);
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
    std::vector<Flight> in_flight;
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
                                                               rhi::Extent2D extent) {
    if (!supported(impl_->device)) {
        return std::unexpected(GpuRasterError::unsupported);
    }
    if (impl_->pending.size() >= impl_->config.max_jobs_per_batch) {
        return std::unexpected(GpuRasterError::batch_full);
    }
    if (extent.width == 0U || extent.height == 0U) {
        return std::unexpected(GpuRasterError::too_large);
    }

    const GpuPathScene scene = gpu_scene_from_shapes(shapes);
    const EncodedGpuScene encoded = encode_gpu_scene(scene, extent.width, extent.height);
    const std::size_t pitch =
        rhi::aligned_texture_copy_pitch(extent.width, rhi::TextureFormat::R8G8B8A8Unorm);
    const auto layout = plan_path_raster(encoded, extent.width, extent.height,
                                         static_cast<std::uint32_t>(pitch / 4U));
    if (!layout || groups(layout->push.nodes_base, kGroupSize) > kMaxGroups ||
        groups(encoded.seg_count, kGroupSize) > kMaxGroups ||
        groups(extent.width, kTile) > kMaxGroups || groups(extent.height, kTile) > kMaxGroups) {
        return std::unexpected(GpuRasterError::too_large);
    }

    try {
        Impl::Job job;
        job.buffers[0] = impl_->storage(encoded.segs, kGpuSegWords);
        job.buffers[1] = impl_->storage(encoded.paths, kGpuPathWords);
        job.buffers[2] = impl_->acquire_buffer(layout->work_words * sizeof(std::uint32_t), true);
        job.buffers[3] = impl_->acquire_buffer(layout->output_words * sizeof(std::uint32_t), true);
        job.buffers[4] = impl_->storage(encoded.strokes, kGpuStrokeWords);
        job.buffers[5] = impl_->storage(encoded.dashes, kGpuDashWords);
        job.layout = *layout;
        job.stroke_count = encoded.stroke_count;
        job.seg_count = encoded.seg_count;
        job.target = &target;
        job.extent = extent;
        job.pitch = pitch;
        impl_->pending.push_back(std::move(job));
    } catch (const std::exception&) {
        return std::unexpected(GpuRasterError::device_failure);
    }
    return {};
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
        Impl::Flight flight;
        flight.list = impl_->device.create_command_list();
        for (const Impl::Job& job : impl_->pending) {
            auto slot = impl_->take_slot();
            if (!slot) {
                for (auto& taken : flight.slots) {
                    impl_->free_slots.push_back(std::move(taken));
                }
                impl_->pending.clear();
                return std::unexpected(GpuRasterError::device_failure);
            }
            impl_->record_job(*flight.list, *slot, job);
            flight.slots.push_back(std::move(slot));
        }
        flight.list->submit_async();
        flight.jobs = std::move(impl_->pending);
        impl_->pending.clear();
        impl_->jobs += flight.jobs.size();
        ++impl_->flushes;
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
        if (!flights[index].list->is_complete()) {
            ++index;
            continue;
        }
        for (auto& slot : flights[index].slots) {
            impl_->free_slots.push_back(std::move(slot));
        }
        for (Impl::Job& job : flights[index].jobs) {
            for (std::size_t binding = 0; binding < job.buffers.size(); ++binding) {
                impl_->release_buffer(std::move(job.buffers[binding]),
                                      binding == 2 || binding == 3);
            }
        }
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
