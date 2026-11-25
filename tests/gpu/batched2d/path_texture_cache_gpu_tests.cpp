// SPDX-License-Identifier: MIT
//
// PathTextureCache with the GPU rasterizer attached (ADR-071), on a real device: a large raster is
// queued on the GPU and its texture holds the pixels once flush_gpu() has run; a small one stays on
// ThorVG; a dense one (here: a bound of one bin node) falls back to the CPU; and a texture evicted
// while its raster was still queued is submitted first and kept alive one more frame. Skips without
// a Vulkan device, without compute, or without the ThorVG backend.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
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

constexpr std::uint32_t kTargetWidth = 160;
constexpr std::uint32_t kTargetHeight = 128;

[[nodiscard]] bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "path_texture_cache_gpu: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] path::PathShape box(float side, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    path::PathShape shape;
    shape.path.segments.push_back({path::SegmentKind::move_to, {0, 0}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {side, 0}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {side, side}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {0, side}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    shape.paint.fill = path::Rgba8{r, g, b, 255};
    return shape;
}

// Draws `texture` 1:1 at the origin on black and returns the target's RGBA bytes.
[[nodiscard]] std::vector<std::uint8_t> draw(rhi::Device& device, rhi::Texture& texture,
                                             rhi::Extent2D extent) {
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
    renderer.push_texture(
        {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height)}, texture,
        {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
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

[[nodiscard]] path::PathTextureKey key_for(std::uint64_t id, std::uint32_t side,
                                           const path::PathShape& shape) {
    return path::make_path_texture_key(id, 1, side, side, 1000, shape);
}

bool run(rhi::Device& device, path::PathRasterizer& cpu) {
    bool ok = true;
    path::GpuPathRasterizer gpu_rasterizer(device, vk::path_raster_pipeline_descs());
    path::PathTextureCacheConfig config;
    config.max_entries = 4;
    path::PathTextureCache cache(device, cpu, config);
    cache.set_gpu_rasterizer(&gpu_rasterizer);

    const path::PathShape red = box(128.0f, 255, 0, 0);
    const auto large = cache.acquire(key_for(1, 128, red), red);
    ok = expect(large.has_value() && large->extent.width == 128, "a large raster is cached") && ok;
    ok = expect(cache.stats().gpu_rasterizations == 1 && cache.stats().rasterizations == 0,
                "the large raster went to the GPU, not to ThorVG") &&
         ok;

    const path::PathShape small_shape = box(64.0f, 0, 255, 0);
    const auto small = cache.acquire(key_for(2, 64, small_shape), small_shape);
    ok = expect(small.has_value() && cache.stats().rasterizations == 1 &&
                    cache.stats().gpu_rasterizations == 1,
                "a 64 px icon stays on ThorVG") &&
         ok;

    ok = expect(cache.flush_gpu().has_value(), "flush_gpu") && ok;
    device.wait_idle();
    cache.end_frame();
    if (large.has_value()) {
        const auto pixels = draw(device, *large->texture, large->extent);
        ok = expect(is(pixels, 64, 64, 255, 0, 0) && is(pixels, 127, 127, 255, 0, 0),
                    "the GPU raster's texture holds its pixels after flush_gpu") &&
             ok;
        ok = expect(is(pixels, 128, 64, 0, 0, 0), "outside the raster is untouched") && ok;
    }
    ok = expect(cache.acquire(key_for(1, 128, red), red).has_value() && cache.stats().hits == 1 &&
                    cache.stats().gpu_rasterizations == 1,
                "a GPU-rasterized texture is a normal cache hit") &&
         ok;

    // A bound of one bin node refuses the large raster: it is rasterized on the CPU, counted.
    {
        path::PathTextureCache dense_cache(device, cpu, config);
        path::RasterRouteConfig bound;
        bound.max_row_nodes = 1;
        dense_cache.set_gpu_rasterizer(&gpu_rasterizer, bound);
        const auto result = dense_cache.acquire(key_for(3, 128, red), red);
        ok = expect(result.has_value() && dense_cache.stats().gpu_fallbacks == 1 &&
                        dense_cache.stats().rasterizations == 1 &&
                        dense_cache.stats().gpu_rasterizations == 0,
                    "a dense raster falls back to the CPU") &&
             ok;
    }

    // A texture evicted while its raster is still queued is submitted first and kept one more
    // frame.
    {
        path::PathTextureCacheConfig one;
        one.max_entries = 1;
        path::PathTextureCache tight(device, cpu, one);
        tight.set_gpu_rasterizer(&gpu_rasterizer);
        const path::PathShape blue = box(128.0f, 0, 0, 255);
        const auto first = tight.acquire(key_for(4, 128, red), red);
        const auto second = tight.acquire(key_for(5, 128, blue), blue);
        ok = expect(first.has_value() && second.has_value() && tight.stats().retired == 1,
                    "the first entry was evicted by the second") &&
             ok;
        ok = expect(gpu_rasterizer.stats().flushes >= 1,
                    "evicting a queued raster submits the batch first") &&
             ok;
        ok = expect(tight.flush_gpu().has_value(), "flush_gpu") && ok;
        device.wait_idle();
        tight.end_frame();
        ok = expect(tight.stats().retired == 1,
                    "the evicted texture survives the frame its copy was submitted in") &&
             ok;
        device.wait_idle();
        tight.end_frame();
        ok = expect(tight.stats().retired == 0, "and is destroyed the frame after") && ok;
        if (second.has_value()) {
            const auto pixels = draw(device, *second->texture, second->extent);
            ok = expect(is(pixels, 64, 64, 0, 0, 255), "the surviving texture holds its pixels") &&
                 ok;
        }
    }
    return ok;
}

} // namespace

int main() {
    if (!path::path_backend_available(path::PathBackend::thorvg)) {
        std::cout << "path_texture_cache_gpu: skipped (ThorVG backend unavailable)\n";
        return 0;
    }
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "path_texture_cache_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!path::GpuPathRasterizer::supported(*device)) {
        std::cout << "path_texture_cache_gpu: skipped (no compute or buffer-to-texture copy)\n";
        return 0;
    }
    const auto cpu = path::make_path_rasterizer(path::PathBackend::thorvg);
    try {
        if (!run(*device, *cpu)) {
            std::cerr << "path_texture_cache_gpu: FAILED\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "path_texture_cache_gpu: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "path_texture_cache_gpu: ok\n";
    return 0;
}
