// SPDX-License-Identifier: MIT
//
// Runs a GPU path scene through the production passes (ADR-071) and reads the straight-alpha RGBA8
// output back, for the ThorVG comparison and the lavapipe goldens (issue #685). The output buffer
// is copied to a readback buffer directly, so the pixels are the rasterizer's own, not a
// composite over a background.

#pragma once

#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/path/gpu_path_scene.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace tessera::path_corpus {

class GpuRaster {
public:
    explicit GpuRaster(rendering::rhi::Device& device)
        : device_(device),
          clear_(device.create_compute_pipeline(
              rendering::vulkan::path_raster_pipeline_desc(rendering::vulkan::PathRasterPass::Clear))),
          expand_(device.create_compute_pipeline(rendering::vulkan::path_raster_pipeline_desc(
              rendering::vulkan::PathRasterPass::Stroke))),
          bin_(device.create_compute_pipeline(
              rendering::vulkan::path_raster_pipeline_desc(rendering::vulkan::PathRasterPass::Bin))),
          fine_(device.create_compute_pipeline(rendering::vulkan::path_raster_pipeline_desc(
              rendering::vulkan::PathRasterPass::Fine))) {}

    // width x height straight-alpha RGBA8 pixels of `scene`, tightly packed.
    [[nodiscard]] std::vector<std::uint8_t> render(const rendering::path::GpuPathScene& scene,
                                                   std::uint32_t width, std::uint32_t height) {
        namespace rhi = rendering::rhi;
        namespace path = rendering::path;
        const path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, width, height);
        const auto layout = path::plan_path_raster(encoded, width, height);
        if (!layout) {
            throw std::runtime_error("plan_path_raster refused the scene");
        }
        const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
        if (layout->output_words * sizeof(std::uint32_t) < pixel_bytes) {
            throw std::runtime_error("the raster output is smaller than the target");
        }
        const auto storage = [&](const std::vector<std::uint32_t>& words, std::size_t minimum) {
            auto buffer = device_.create_buffer(
                {std::max(words.size(), minimum) * sizeof(std::uint32_t), rhi::BufferUsage::Storage});
            if (!words.empty()) {
                buffer->write(words.data(), words.size() * sizeof(std::uint32_t));
            }
            return buffer;
        };
        const auto segs = storage(encoded.segs, path::kGpuSegWords);
        const auto paths = storage(encoded.paths, path::kGpuPathWords);
        const auto strokes = storage(encoded.strokes, path::kGpuStrokeWords);
        const auto dashes = storage(encoded.dashes, path::kGpuDashWords);
        rhi::BufferDesc read_write{layout->work_words * sizeof(std::uint32_t),
                                   rhi::BufferUsage::Storage};
        read_write.gpu_read_write = true;
        const auto work = device_.create_buffer(read_write);
        read_write.size = layout->output_words * sizeof(std::uint32_t);
        const auto output = device_.create_buffer(read_write);
        const auto readback = device_.create_buffer({pixel_bytes, rhi::BufferUsage::Readback});
        const path::PathRasterJob job_record = path::single_raster_job(*layout);
        const auto jobs = storage(std::vector<std::uint32_t>(path::kPathRasterJobWords, 0), 1);
        jobs->write(&job_record, sizeof(job_record));
        const std::uint32_t job_base = 0;

        auto list = device_.create_command_list();
        const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
            list->bind_compute_pipeline(pipeline);
            list->bind_compute_buffer(0, *segs);
            list->bind_compute_buffer(1, *paths);
            list->bind_compute_buffer(2, *work);
            list->bind_compute_buffer(3, *output);
            list->bind_compute_buffer(4, *strokes);
            list->bind_compute_buffer(5, *dashes);
            list->bind_compute_buffer(6, *jobs);
            list->compute_push_constants(&job_base, sizeof(job_base));
        };
        const auto groups = [](std::uint32_t count, std::uint32_t size) {
            return (count + size - 1) / size;
        };
        bind_all(*clear_);
        list->dispatch(groups(layout->push.nodes_base, 256), 1, 1);
        list->compute_barrier();
        if (encoded.stroke_count > 0) {
            bind_all(*expand_);
            list->dispatch(groups(encoded.stroke_count, 256), 1, 1);
            list->compute_barrier();
        }
        bind_all(*bin_);
        list->dispatch(groups(std::max(encoded.seg_count, 1U), 256), 1, 1);
        list->compute_barrier();
        bind_all(*fine_);
        list->dispatch(groups(width, 16), groups(height, 16), 1);
        list->copy_buffer(*output, *readback, pixel_bytes);
        list->submit();
        device_.wait_idle();
        std::vector<std::uint8_t> pixels(pixel_bytes);
        readback->read(pixels.data(), pixels.size());
        return pixels;
    }

private:
    rendering::rhi::Device& device_;
    std::unique_ptr<rendering::rhi::ComputePipeline> clear_;
    std::unique_ptr<rendering::rhi::ComputePipeline> expand_;
    std::unique_ptr<rendering::rhi::ComputePipeline> bin_;
    std::unique_ptr<rendering::rhi::ComputePipeline> fine_;
};

} // namespace tessera::path_corpus
