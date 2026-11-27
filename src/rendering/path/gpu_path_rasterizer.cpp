// SPDX-License-Identifier: MIT

#include "gpu_path_rasterizer_impl.hpp"

namespace tessera::rendering::path {

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
    if (extent.width == 0U || extent.height == 0U) {
        return std::unexpected(GpuRasterError::too_large);
    }
    auto scene = std::make_shared<EncodedGpuScene>(
        encode_gpu_scene(gpu_scene_from_shapes(shapes), extent.width, extent.height));
    std::vector<std::uint32_t> paths = std::move(scene->paths);
    scene->paths.clear();
    return impl_->queue(std::move(scene), std::move(paths), 1.0f, target, extent, max_row_nodes,
                        false);
}

std::expected<void, GpuRasterError>
GpuPathRasterizer::enqueue(std::shared_ptr<const GpuPathDocument> document, float scale,
                           rhi::Texture& target, rhi::Extent2D extent,
                           std::uint64_t max_row_nodes) {
    if (!document || extent.width == 0U || extent.height == 0U || !(scale > 0.0f) ||
        scale > document->max_scale) {
        return std::unexpected(GpuRasterError::too_large);
    }
    const PathAffine view = document_view(scale);
    std::vector<PathAffine> matrices(document->encoded.path_count, view);
    std::vector<std::uint32_t> paths =
        make_path_records(document->encoded, matrices, extent.width, extent.height);
    std::shared_ptr<const EncodedGpuScene> scene(document, &document->encoded);
    return impl_->queue(std::move(scene), std::move(paths), document->max_scale, target, extent,
                        max_row_nodes, true);
}

std::expected<void, GpuRasterError> GpuPathRasterizer::Impl::queue(
    std::shared_ptr<const EncodedGpuScene> scene, std::vector<std::uint32_t> paths, float max_scale,
    rhi::Texture& target, rhi::Extent2D extent, std::uint64_t max_row_nodes, bool resident_ok) {
    if (!GpuPathRasterizer::supported(device)) {
        return std::unexpected(GpuRasterError::unsupported);
    }
    if (pending.size() >= config.max_jobs_per_batch) {
        return std::unexpected(GpuRasterError::batch_full);
    }
    Job job;
    job.scene = std::move(scene);
    job.paths = std::move(paths);
    job.pitch = rhi::aligned_texture_copy_pitch(extent.width, rhi::TextureFormat::R8G8B8A8Unorm);
    const auto layout = plan_path_raster(*job.scene, extent.width, extent.height,
                                         static_cast<std::uint32_t>(job.pitch / 4U), max_scale);
    if (!layout || groups(layout->push.nodes_base, kGroupSize) > kMaxGroups ||
        groups(job.scene->seg_count, kGroupSize) > kMaxGroups ||
        groups(extent.width, kTile) > kMaxGroups || groups(extent.height, kTile) > kMaxGroups) {
        return std::unexpected(GpuRasterError::too_large);
    }
    if (max_row_nodes != 0 && job.scene->row_nodes > max_row_nodes) {
        return std::unexpected(GpuRasterError::too_dense);
    }
    job.layout = *layout;
    job.target = &target;
    job.extent = extent;
    if (resident_ok) {
        make_resident(job);
    }
    if (!fits(job)) {
        return std::unexpected(GpuRasterError::batch_full);
    }
    pending.push_back(std::move(job));
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
    stats.uploaded_bytes = impl_->uploaded_bytes;
    if (impl_->arena) {
        const auto arena = impl_->arena->stats();
        stats.uploaded_bytes += arena.uploaded_bytes;
        stats.resident_uploads = arena.uploads;
        stats.resident_hits = arena.hits;
        stats.resident_documents = arena.documents;
    }
    return stats;
}

} // namespace tessera::rendering::path
