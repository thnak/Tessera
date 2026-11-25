// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/gpu_path_scene.hpp"
#include "tessera/rendering/rhi/texture_region.hpp"

#include <algorithm>
#include <array>
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

    [[nodiscard]] std::unique_ptr<rhi::Buffer> storage(const std::vector<std::uint32_t>& words,
                                                       std::size_t minimum_words) {
        auto buffer =
            device.create_buffer({std::max(words.size(), minimum_words) * sizeof(std::uint32_t),
                                  rhi::BufferUsage::Storage});
        if (!words.empty()) {
            buffer->write(words.data(), words.size() * sizeof(std::uint32_t));
        }
        return buffer;
    }

    void record_job(rhi::CommandList& list, const Slot& slot, const Job& job) {
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
            list.bind_compute_pipeline(pipeline);
            for (std::uint32_t binding = 0; binding < job.buffers.size(); ++binding) {
                list.bind_compute_buffer(binding, *job.buffers[binding]);
            }
            list.compute_push_constants(&job.layout.push, sizeof(job.layout.push));
        };
        bind_all(*slot.clear);
        list.dispatch(groups(job.layout.push.nodes_base, kGroupSize), 1, 1);
        list.compute_barrier();
        if (job.stroke_count > 0) {
            bind_all(*slot.stroke);
            list.dispatch(groups(job.stroke_count, kGroupSize), 1, 1);
            list.compute_barrier();
        }
        bind_all(*slot.bin);
        list.dispatch(groups(std::max(job.seg_count, 1U), kGroupSize), 1, 1);
        list.compute_barrier();
        bind_all(*slot.fine);
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
        rhi::BufferDesc read_write{layout->work_words * sizeof(std::uint32_t),
                                   rhi::BufferUsage::Storage};
        read_write.gpu_read_write = true;
        job.buffers[2] = impl_->device.create_buffer(read_write);
        read_write.size = layout->output_words * sizeof(std::uint32_t);
        job.buffers[3] = impl_->device.create_buffer(read_write);
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
    return stats;
}

} // namespace tessera::rendering::path
