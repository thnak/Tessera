// SPDX-License-Identifier: MIT
//
// GpuPathRasterizer (ADR-071): several rasters queued, one flush, one submission, one dispatch of
// each compute pass for all of them (the shader's job table). Three rectangles and a stroked line
// of different sizes are rasterized into four textures in a single batch, drawn side by side and
// read back; later batches check that the pipeline set and the buffers are reused once the first
// flight completed, that two flights in flight need two sets (a set's descriptors are rewritten by
// every bind), and that warm() and the batch bound behave. Skips without a Vulkan device or without
// compute.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/path_scale.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

void wait_collect(path::GpuPathRasterizer& rasterizer, rhi::Device& device) {
    device.wait_idle();
    rasterizer.collect();
}

[[nodiscard]] path::PathShape stroked_line() {
    path::PathShape shape;
    shape.path.segments.push_back({path::SegmentKind::move_to, {2, 6}, {}, {}});
    shape.path.segments.push_back({path::SegmentKind::line_to, {26, 6}, {}, {}});
    path::StrokeStyle stroke;
    stroke.color = {255, 255, 0, 255};
    stroke.width = 4.0f;
    stroke.cap = path::LineCap::butt;
    shape.paint.stroke = stroke;
    return shape;
}

[[nodiscard]] bool run(rhi::Device& device) {
    path::GpuPathRasterizer rasterizer(device, vk::path_raster_pipeline_descs());
    // Different sizes, and one raster with a stroke, so the batch's dispatch grid (the largest
    // raster by the job count) has threads past a raster's own extent and the stroke pass has jobs
    // with no stroke at all.
    const std::array<rhi::Extent2D, 4> extents{{{32, 32}, {24, 16}, {30, 20}, {28, 12}}};
    const std::array<path::PathShape, 4> shapes{box(32, 32, 255, 0, 0), box(24, 16, 0, 255, 0),
                                                box(30, 20, 0, 0, 255), stroked_line()};

    std::vector<Raster> rasters;
    bool ok = true;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        rasters.push_back(blank_texture(device, extents[i]));
        const auto queued = rasterizer.enqueue(std::span<const path::PathShape>(&shapes[i], 1),
                                               *rasters.back().texture, extents[i]);
        ok = expect(queued.has_value(), "enqueue " + std::to_string(i)) && ok;
    }
    ok = expect(rasterizer.stats().pending == 4 && rasterizer.stats().flushes == 0,
                "nothing is submitted before flush") &&
         ok;
    ok = expect(rasterizer.flush().has_value(), "flush") && ok;
    ok = expect(rasterizer.stats().flushes == 1 && rasterizer.stats().jobs == 4 &&
                    rasterizer.stats().pipeline_slots == 1,
                "four rasters, one submission, one pipeline set") &&
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
    ok = expect(is(pixels, 120 + 14, 5, 255, 255, 0) && is(pixels, 120 + 14, 7, 255, 255, 0) &&
                    is(pixels, 120 + 14, 3, 0, 0, 0) && is(pixels, 120 + 14, 9, 0, 0, 0),
                "fourth raster is its stroked line: the stroke pass ran for that job only") &&
         ok;
    ok = expect(is(pixels, 40 + 24, 8, 0, 0, 0) && is(pixels, 50, 16, 0, 0, 0),
                "pixels past each raster's own extent are untouched") &&
         ok;

    // A second batch reuses the pipeline set and the buffers of the first, which completed.
    const path::PathShape yellow = box(24.0f, 16.0f, 255, 255, 0);
    auto again = blank_texture(device, {24, 16});
    ok = expect(rasterizer
                    .enqueue(std::span<const path::PathShape>(&yellow, 1), *again.texture,
                             again.extent)
                    .has_value(),
                "second enqueue") &&
         expect(rasterizer.flush().has_value(), "second flush") && ok;
    ok = expect(rasterizer.stats().pipeline_slots == 1, "the second batch reuses the set") && ok;
    ok = expect(rasterizer.stats().buffers_reused >= 7,
                "the second batch takes its buffers from the pool, not the device") &&
         ok;
    wait_collect(rasterizer, device);

    // Two batches in flight at once need two pipeline sets: a set's descriptors are rewritten by
    // every bind, so a running batch's set is not handed out.
    path::GpuPathRasterizer overlapped(device, vk::path_raster_pipeline_descs());
    for (int batch = 0; batch < 2; ++batch) {
        ok = expect(overlapped
                            .enqueue(std::span<const path::PathShape>(&yellow, 1), *again.texture,
                                     again.extent)
                            .has_value() &&
                        overlapped.flush().has_value(),
                    "overlapped batch") &&
             ok;
    }
    ok = expect(overlapped.stats().pipeline_slots == 2, "two batches in flight, two sets") && ok;
    wait_collect(overlapped, device);

    // warm() builds sets ahead of the first batch and is idempotent.
    path::GpuPathRasterizer warmed(device, vk::path_raster_pipeline_descs());
    ok = expect(warmed.warm(2).has_value() && warmed.stats().pipeline_slots == 2, "warm(2)") && ok;
    ok = expect(warmed.warm(1).has_value() && warmed.stats().pipeline_slots == 2,
                "warm never shrinks or rebuilds") &&
         ok;
    ok = expect(warmed.enqueue(std::span<const path::PathShape>(&yellow, 1), *again.texture,
                               again.extent)
                        .has_value() &&
                    warmed.flush().has_value() && warmed.stats().pipeline_slots == 2,
                "a warmed rasterizer builds no set for its first batch") &&
         ok;
    wait_collect(warmed, device);

    // A full batch is refused, not dropped.
    path::GpuPathRasterizerConfig small;
    small.max_jobs_per_batch = 1;
    path::GpuPathRasterizer tiny(device, vk::path_raster_pipeline_descs(), small);
    ok = expect(tiny.enqueue(std::span<const path::PathShape>(&yellow, 1), *again.texture,
                             again.extent)
                    .has_value(),
                "first job fits") &&
         ok;
    const auto refused =
        tiny.enqueue(std::span<const path::PathShape>(&yellow, 1), *again.texture, again.extent);
    ok = expect(!refused.has_value() && refused.error() == path::GpuRasterError::batch_full,
                "a second job in a one-job batch is refused with batch_full") &&
         ok;
    ok = expect(tiny.flush().has_value(), "flush the single job") && ok;
    wait_collect(tiny, device);

    // A scale-independent document at 3x equals the legacy route (shapes pre-scaled, encoded for
    // the target): the scale is applied on the GPU through the per-path affine.
    {
        const std::array<path::PathShape, 2> doc_shapes{box(12.0f, 8.0f, 200, 40, 90),
                                                        stroked_line()};
        const rhi::Extent2D big{36, 24};
        const auto scaled = path::scale_path_shapes(doc_shapes, 3.0f);
        std::vector<Raster> pair;
        pair.push_back(blank_texture(device, big));
        pair.push_back(blank_texture(device, big));
        const auto document = path::make_gpu_document(doc_shapes, 4.0f);
        path::GpuPathRasterizer docs(device, vk::path_raster_pipeline_descs());
        ok = expect(docs.enqueue(scaled, *pair[0].texture, big).has_value() &&
                        docs.enqueue(document, 3.0f, *pair[1].texture, big).has_value() &&
                        docs.flush().has_value(),
                    "legacy and document rasters queue") &&
             ok;
        wait_collect(docs, device);
        const auto both = draw(device, pair);
        std::size_t worst = 0;
        for (std::uint32_t y = 0; y < big.height; ++y) {
            for (std::uint32_t x = 0; x < big.width; ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    const auto at = [&](std::uint32_t px) {
                        return both[(static_cast<std::size_t>(y) * kTargetWidth + px) * 4U + c];
                    };
                    const int diff = std::abs(int{at(x)} - int{at(40 + x)});
                    worst = std::max<std::size_t>(worst, static_cast<std::size_t>(diff));
                }
            }
        }
        // The document flattens at 0.05/4 document units and the legacy shapes at 0.05 pixels: the
        // same curve tolerance, so straight-edged shapes agree exactly and curves within a level.
        ok = expect(worst <= 2, "a document at 3x matches the pre-scaled shapes (worst diff " +
                                    std::to_string(worst) + ")") &&
             ok;
        ok = expect(is(both, 20, 5, 200, 40, 90), "the scaled document fills its rectangle") && ok;
        // The document is resident after its first raster: a second raster at another scale, in a
        // later flush, uploads only its job record and path records.
        const auto before = docs.stats();
        ok = expect(before.resident_uploads == 1 && before.resident_documents == 1,
                    "the document was uploaded to the arena once") &&
             ok;
        auto other = blank_texture(device, {24, 16});
        ok = expect(docs.enqueue(document, 2.0f, *other.texture, {24, 16}).has_value() &&
                        docs.flush().has_value(),
                    "a second scale of the same document") &&
             ok;
        wait_collect(docs, device);
        const auto after = docs.stats();
        const std::uint64_t expected_bytes =
            sizeof(path::PathRasterJob) + document->encoded.path_count * path::kGpuPathWords * 4U;
        ok =
            expect(after.resident_uploads == 1 && after.resident_hits == before.resident_hits + 1 &&
                       after.uploaded_bytes - before.uploaded_bytes == expected_bytes,
                   "a resident raster uploads only its job and path records (" +
                       std::to_string(after.uploaded_bytes - before.uploaded_bytes) + " vs " +
                       std::to_string(expected_bytes) + ")") &&
            ok;
        // A batch mixing a resident and a legacy raster still renders both correctly.
        auto mixed = blank_texture(device, big);
        auto mixed_legacy = blank_texture(device, big);
        ok = expect(docs.enqueue(document, 3.0f, *mixed.texture, big).has_value() &&
                        docs.enqueue(scaled, *mixed_legacy.texture, big).has_value() &&
                        docs.flush().has_value(),
                    "mixed resident and legacy batch") &&
             ok;
        wait_collect(docs, device);
        std::vector<Raster> mixed_pair;
        mixed_pair.push_back(std::move(mixed));
        mixed_pair.push_back(std::move(mixed_legacy));
        ok = expect(draw(device, mixed_pair) == both, "a mixed batch matches the resident batch") &&
             ok;
        ok = expect(!docs.enqueue(document, 8.0f, *pair[1].texture, big).has_value(),
                    "a scale above the document's bound is refused") &&
             ok;
    }
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
