// SPDX-License-Identifier: MIT
//
// GpuPathRasterizer (ADR-071): several rasters queued, one flush, one submission. Three solid
// rectangles of different colors and sizes are rasterized into three textures in a single batch,
// drawn side by side and read back; then a second batch checks that the job slots (a compute
// pipeline owns its descriptor set, so a slot cannot be rebound while in flight) are reused once
// the first flight completed. Skips without a Vulkan device or without compute.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gpu = tessera::rendering::gpu;
namespace path = tessera::rendering::path;
namespace vk = tessera::rendering::vulkan;

constexpr std::uint32_t kTargetWidth = 128;
constexpr std::uint32_t kTargetHeight = 32;

[[nodiscard]] bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "path_raster_batch_gpu: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] path::PathShape box(float x1, float y1, std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b) {
    path::PathShape shape;
    shape.path.segments.push_back({path::SegmentKind::move_to, {0, 0}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {x1, 0}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {0, y1}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    shape.paint.fill = path::Rgba8{r, g, b, 255};
    return shape;
}

struct Raster {
    std::unique_ptr<rhi::Texture> texture;
    rhi::Extent2D extent;
};

[[nodiscard]] Raster blank_texture(rhi::Device& device, rhi::Extent2D extent) {
    rhi::TextureDesc desc;
    desc.extent = extent;
    const std::vector<std::uint8_t> blank(
        static_cast<std::size_t>(extent.width) * extent.height * 4U, 0);
    desc.pixels = blank.data();
    desc.row_pitch = static_cast<std::size_t>(extent.width) * 4U;
    return {device.create_texture(desc), extent};
}

// Draws each texture 1:1 at x = 0, 40, 80 on black and returns the target's RGBA bytes.
[[nodiscard]] std::vector<std::uint8_t> draw(rhi::Device& device,
                                             const std::vector<Raster>& rasters) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kTargetWidth, kTargetHeight};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    auto target = device.create_render_target(target_desc);
    auto pipeline_desc = vk::batched2d_pipeline_desc();
    pipeline_desc.color_format = target_desc.format;
    auto pipeline = device.create_pipeline(pipeline_desc);
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);
    gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto list = device.create_command_list();
    renderer.begin(*list, *target, target_desc);
    float x = 0.0f;
    for (const Raster& raster : rasters) {
        renderer.push_texture({x, 0.0f, static_cast<float>(raster.extent.width),
                               static_cast<float>(raster.extent.height)},
                              *raster.texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
        x += 40.0f;
    }
    renderer.end();
    list->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(kTargetWidth) * kTargetHeight * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy_back = device.create_command_list();
    copy_back->copy_target_to_buffer(*target, *readback);
    copy_back->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

[[nodiscard]] bool is(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y,
                      std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::size_t i = (static_cast<std::size_t>(y) * kTargetWidth + x) * 4U;
    return pixels[i] == r && pixels[i + 1] == g && pixels[i + 2] == b;
}

[[nodiscard]] path::GpuPathPipelineDescs descs() {
    return {vk::path_raster_pipeline_desc(vk::PathRasterPass::Clear),
            vk::path_raster_pipeline_desc(vk::PathRasterPass::Stroke),
            vk::path_raster_pipeline_desc(vk::PathRasterPass::Bin),
            vk::path_raster_pipeline_desc(vk::PathRasterPass::Fine)};
}

void wait_collect(path::GpuPathRasterizer& rasterizer, rhi::Device& device) {
    device.wait_idle();
    rasterizer.collect();
}

[[nodiscard]] bool run(rhi::Device& device) {
    path::GpuPathRasterizer rasterizer(device, descs());
    const std::array<rhi::Extent2D, 3> extents{{{32, 32}, {24, 16}, {30, 20}}};
    const std::array<std::array<std::uint8_t, 3>, 3> colors{
        {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}}};

    std::vector<Raster> rasters;
    bool ok = true;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        rasters.push_back(blank_texture(device, extents[i]));
        const path::PathShape shape =
            box(static_cast<float>(extents[i].width), static_cast<float>(extents[i].height),
                colors[i][0], colors[i][1], colors[i][2]);
        const auto queued = rasterizer.enqueue(std::span<const path::PathShape>(&shape, 1),
                                               *rasters.back().texture, extents[i]);
        ok = expect(queued.has_value(), "enqueue " + std::to_string(i)) && ok;
    }
    ok = expect(rasterizer.stats().pending == 3 && rasterizer.stats().flushes == 0,
                "nothing is submitted before flush") &&
         ok;
    ok = expect(rasterizer.flush().has_value(), "flush") && ok;
    ok = expect(rasterizer.stats().flushes == 1 && rasterizer.stats().jobs == 3 &&
                    rasterizer.stats().pipeline_slots == 3,
                "three rasters, one submission, three slots") &&
         ok;
    wait_collect(rasterizer, device);
    ok = expect(rasterizer.stats().in_flight == 0, "a finished flight is collected") && ok;

    const auto pixels = draw(device, rasters);
    ok = expect(is(pixels, 10, 10, 255, 0, 0) && is(pixels, 31, 31, 255, 0, 0),
                "first raster is its red rectangle") &&
         ok;
    ok = expect(is(pixels, 50, 8, 0, 255, 0) && is(pixels, 40 + 23, 15, 0, 255, 0),
                "second raster is its green rectangle") &&
         ok;
    ok = expect(is(pixels, 90, 10, 0, 0, 255) && is(pixels, 80 + 29, 19, 0, 0, 255),
                "third raster is its blue rectangle") &&
         ok;
    ok = expect(is(pixels, 40 + 24, 8, 0, 0, 0) && is(pixels, 50, 16, 0, 0, 0),
                "pixels past each raster's own extent are untouched") &&
         ok;

    // A second batch reuses the slots of the first, which completed: no new pipelines.
    const path::PathShape shape = box(24.0f, 16.0f, 255, 255, 0);
    auto again = blank_texture(device, {24, 16});
    ok = expect(
             rasterizer
                 .enqueue(std::span<const path::PathShape>(&shape, 1), *again.texture, again.extent)
                 .has_value(),
             "second enqueue") &&
         expect(rasterizer.flush().has_value(), "second flush") && ok;
    ok =
        expect(rasterizer.stats().pipeline_slots == 3, "the second batch reuses a free slot") && ok;
    ok = expect(rasterizer.stats().buffers_reused >= 6,
                "the second batch takes its buffers from the pool, not the device") &&
         ok;
    wait_collect(rasterizer, device);

    // warm() builds slots ahead of the first batch and is idempotent.
    path::GpuPathRasterizer warmed(device, descs());
    ok = expect(warmed.warm(4).has_value() && warmed.stats().pipeline_slots == 4, "warm(4)") && ok;
    ok = expect(warmed.warm(2).has_value() && warmed.stats().pipeline_slots == 4,
                "warm never shrinks or rebuilds") &&
         ok;
    ok = expect(warmed.enqueue(std::span<const path::PathShape>(&shape, 1), *again.texture,
                               again.extent)
                        .has_value() &&
                    warmed.flush().has_value() && warmed.stats().pipeline_slots == 4,
                "a warmed rasterizer builds no slot for its first batch") &&
         ok;
    wait_collect(warmed, device);

    // A full batch is refused, not dropped.
    path::GpuPathRasterizerConfig small;
    small.max_jobs_per_batch = 1;
    path::GpuPathRasterizer tiny(device, descs(), small);
    ok = expect(
             tiny.enqueue(std::span<const path::PathShape>(&shape, 1), *again.texture, again.extent)
                 .has_value(),
             "first job fits") &&
         ok;
    const auto refused =
        tiny.enqueue(std::span<const path::PathShape>(&shape, 1), *again.texture, again.extent);
    ok = expect(!refused.has_value() && refused.error() == path::GpuRasterError::batch_full,
                "a second job in a one-job batch is refused with batch_full") &&
         ok;
    ok = expect(tiny.flush().has_value(), "flush the single job") && ok;
    wait_collect(tiny, device);
    return ok;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_raster_batch_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!path::GpuPathRasterizer::supported(*device)) {
        std::cout << "path_raster_batch_gpu: skipped (no compute or buffer-to-texture copy)\n";
        return 0;
    }
    try {
        if (!run(*device)) {
            std::cerr << "path_raster_batch_gpu: FAILED\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "path_raster_batch_gpu: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "path_raster_batch_gpu: ok\n";
    return 0;
}
