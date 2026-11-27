// SPDX-License-Identifier: MIT
//
// Packing a batch (GpuPathRasterizer::Impl::pack_and_record): the shared buffers, the job table and
// the recorded passes.

#include "gpu_path_rasterizer_impl.hpp"

namespace tessera::rendering::path {

// Packs the pending jobs into the shared buffers and records the four passes once for all of
// them, then one copy per job into its texture. Returns the flight, or nothing on a device
// error.
std::unique_ptr<GpuPathRasterizer::Impl::Flight> GpuPathRasterizer::Impl::pack_and_record() {
    const std::uint32_t count = static_cast<std::uint32_t>(pending.size());
    const Totals total = totals_of(pending);
    const bool all_resident = arena && std::all_of(pending.begin(), pending.end(),
                                                   [](const Job& job) { return job.resident; });
    const auto words = [](std::uint64_t value, std::size_t minimum) {
        return std::max<std::size_t>(static_cast<std::size_t>(value), minimum) *
               sizeof(std::uint32_t);
    };

    auto flight = std::make_unique<Flight>();
    flight->jobs = count;
    flight->uses_arena = all_resident;
    if (!all_resident) {
        flight->buffers[kSegs] = acquire_buffer(words(total.segs, kGpuSegWords), false);
        flight->buffers[kStrokes] = acquire_buffer(words(total.strokes, kGpuStrokeWords), false);
        flight->buffers[kDashes] = acquire_buffer(words(total.dashes, kGpuDashWords), false);
    }
    flight->buffers[kPaths] = acquire_buffer(words(total.paths, kGpuPathWords), false);
    flight->buffers[kWork] = acquire_buffer(words(total.work, 1), true);
    flight->buffers[kOutput] = acquire_buffer(words(total.output, 1), true);
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
            uploaded_bytes += data.size() * sizeof(std::uint32_t);
            flight->buffers[binding]->write(data.data(), data.size() * sizeof(std::uint32_t),
                                            static_cast<std::size_t>(at) * sizeof(std::uint32_t));
        }
    };
    for (std::uint32_t index = 0; index < count; ++index) {
        const Job& job = pending[index];
        PathRasterJob& record = records[index];
        record.push = job.layout.push;
        record.seg_base =
            job.resident && all_resident ? job.slice.segs : static_cast<std::uint32_t>(base.segs);
        record.path_base = static_cast<std::uint32_t>(base.paths);
        record.work_base = static_cast<std::uint32_t>(base.work);
        record.out_base = static_cast<std::uint32_t>(base.output);
        record.stroke_base = job.resident && all_resident
                                 ? job.slice.strokes
                                 : static_cast<std::uint32_t>(base.strokes);
        record.dash_base = job.resident && all_resident ? job.slice.dashes
                                                        : static_cast<std::uint32_t>(base.dashes);
        if (!all_resident) {
            put(kSegs, job.scene->segs, base.segs);
            put(kStrokes, job.scene->strokes, base.strokes);
            put(kDashes, job.scene->dashes, base.dashes);
        }
        put(kPaths, job.paths, base.paths);
        base.segs += job.scene->segs.size();
        base.paths += job.paths.size();
        base.strokes += job.scene->strokes.size();
        base.dashes += job.scene->dashes.size();
        base.work += job.layout.work_words;
        base.output += job.layout.output_words;
        max_clear = std::max(max_clear, job.layout.push.nodes_base);
        max_stroke = std::max(max_stroke, job.scene->stroke_count);
        max_seg = std::max(max_seg, job.scene->seg_count);
        max_width = std::max(max_width, job.extent.width);
        max_height = std::max(max_height, job.extent.height);
    }
    uploaded_bytes += records.size() * sizeof(PathRasterJob);
    flight->buffers[kJobs]->write(records.data(), records.size() * sizeof(PathRasterJob));

    flight->slot = take_slot();
    if (!flight->slot) {
        retire(*flight);
        return nullptr;
    }
    flight->list = device.create_command_list();
    rhi::CommandList& list = *flight->list;
    const std::uint32_t job_base = 0;
    // Every pass binds all seven buffers, though each declares only some of them: a WebGPU compute
    // pipeline's bind group layout holds every slot and a dispatch with one unbound throws
    // (docs/design/gpu/compute.md section 3.1). Vulkan ignores the extras.
    const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
        list.bind_compute_pipeline(pipeline);
        for (std::uint32_t binding = 0; binding < kBufferCount; ++binding) {
            const bool resident = flight->uses_arena &&
                                  (binding == kSegs || binding == kStrokes || binding == kDashes);
            list.bind_compute_buffer(binding,
                                     resident ? arena->buffer(binding) : *flight->buffers[binding]);
        }
        list.compute_push_constants(&job_base, sizeof(job_base));
    };
    bind_all(*flight->slot->clear);
    list.dispatch(groups(max_clear, kGroupSize), count, 1);
    list.compute_barrier();
    if (max_stroke > 0) {
        bind_all(*flight->slot->stroke);
        list.dispatch(groups(max_stroke, kGroupSize), count, 1);
        list.compute_barrier();
    }
    bind_all(*flight->slot->bin);
    list.dispatch(groups(std::max(max_seg, 1U), kGroupSize), count, 1);
    list.compute_barrier();
    bind_all(*flight->slot->fine);
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

} // namespace tessera::rendering::path
