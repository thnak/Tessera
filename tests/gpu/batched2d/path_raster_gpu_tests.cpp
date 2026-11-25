// SPDX-License-Identifier: MIT
//
// The GPU path rasterizer end to end through the production pieces (ADR-071, ADR-072): decoded
// shapes -> `gpu_scene_from_shapes` -> `encode_gpu_scene` -> the four compute passes of
// `path_raster_pipeline_desc` -> `copy_buffer_to_texture` -> a texture the batched renderer draws,
// read back from the render target. The output rows are padded to the copy pitch (100 texels = 400
// bytes -> 512), so the shader's `out_stride` and the copy's pitch have to agree for a pixel to
// land where the test looks. Opaque colors at interior pixels are exact. Skips without a Vulkan
// device or without compute.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
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

constexpr std::uint32_t kWidth = 100;
constexpr std::uint32_t kHeight = 40;

[[nodiscard]] bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "path_raster_gpu: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] path::Path rect(float x0, float y0, float x1, float y1) {
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {x0, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x0, y1}, {}, {}});
    result.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    return result;
}

[[nodiscard]] path::Path line(float x0, float y0, float x1, float y1) {
    path::Path result;
    result.segments.push_back({path::SegmentKind::move_to, {x0, y0}, {}, {}});
    result.segments.push_back({path::SegmentKind::line_to, {x1, y1}, {}, {}});
    return result;
}

// Runs the four passes over `scene` into a buffer whose rows are copy-aligned, copies it into a
// kWidth x kHeight texture, draws that 1:1 on black and returns the target's RGBA bytes.
[[nodiscard]] std::vector<std::uint8_t> rasterize(rhi::Device& device,
                                                  const path::GpuPathScene& scene) {
    const path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, kWidth, kHeight);
    const std::size_t pitch =
        rhi::aligned_texture_copy_pitch(kWidth, rhi::TextureFormat::R8G8B8A8Unorm);
    const auto layout =
        path::plan_path_raster(encoded, kWidth, kHeight, static_cast<std::uint32_t>(pitch / 4U));
    if (!layout) {
        throw std::runtime_error("plan_path_raster refused the scene");
    }
    const auto storage = [&](const std::vector<std::uint32_t>& words, std::size_t minimum_words) {
        auto buffer =
            device.create_buffer({std::max(words.size(), minimum_words) * sizeof(std::uint32_t),
                                  rhi::BufferUsage::Storage});
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
    const auto work = device.create_buffer(read_write);
    read_write.size = layout->output_words * sizeof(std::uint32_t);
    const auto output = device.create_buffer(read_write);

    const path::PathRasterJob job_record = path::single_raster_job(*layout);
    const auto jobs = storage(std::vector<std::uint32_t>(path::kPathRasterJobWords, 0), 1);
    jobs->write(&job_record, sizeof(job_record));
    const std::uint32_t job_base = 0;

    const auto clear =
        device.create_compute_pipeline(vk::path_raster_pipeline_desc(vk::PathRasterPass::Clear));
    const auto expand =
        device.create_compute_pipeline(vk::path_raster_pipeline_desc(vk::PathRasterPass::Stroke));
    const auto bin =
        device.create_compute_pipeline(vk::path_raster_pipeline_desc(vk::PathRasterPass::Bin));
    const auto fine =
        device.create_compute_pipeline(vk::path_raster_pipeline_desc(vk::PathRasterPass::Fine));

    rhi::TextureDesc texture_desc;
    texture_desc.extent = {kWidth, kHeight};
    const std::vector<std::uint8_t> blank(static_cast<std::size_t>(kWidth) * kHeight * 4U, 0);
    texture_desc.pixels = blank.data();
    texture_desc.row_pitch = static_cast<std::size_t>(kWidth) * 4U;
    auto texture = device.create_texture(texture_desc);

    auto list = device.create_command_list();
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
    bind_all(*clear);
    list->dispatch(groups(layout->push.nodes_base, 256), 1, 1);
    list->compute_barrier();
    if (encoded.stroke_count > 0) {
        bind_all(*expand);
        list->dispatch(groups(encoded.stroke_count, 256), 1, 1);
        list->compute_barrier();
    }
    bind_all(*bin);
    list->dispatch(groups(std::max(encoded.seg_count, 1U), 256), 1, 1);
    list->compute_barrier();
    bind_all(*fine);
    list->dispatch(groups(kWidth, 16), groups(kHeight, 16), 1);
    rhi::BufferTextureCopy copy;
    copy.region = {0, 0, kWidth, kHeight};
    copy.buffer_row_pitch = pitch;
    list->copy_buffer_to_texture(*output, *texture, copy);
    list->submit();
    device.wait_idle();

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kWidth, kHeight};
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
    auto draw = device.create_command_list();
    renderer.begin(*draw, *target, target_desc);
    renderer.push_texture({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)},
                          *texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    draw->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy_back = device.create_command_list();
    copy_back->copy_target_to_buffer(*target, *readback);
    copy_back->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

struct Pixel {
    std::uint8_t r, g, b;
};

[[nodiscard]] Pixel at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4U;
    return {pixels[i], pixels[i + 1], pixels[i + 2]};
}

[[nodiscard]] bool is(Pixel p, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return p.r == r && p.g == g && p.b == b;
}

bool run(rhi::Device& device) {
    path::PathShape box;
    box.path = rect(10, 6, 60, 26);
    box.paint.fill = path::Rgba8{255, 0, 0, 255};
    path::PathShape rule;
    rule.path = line(10, 34, 90, 34);
    path::StrokeStyle stroke;
    stroke.color = {0, 0, 255, 255};
    stroke.width = 4.0f;
    stroke.cap = path::LineCap::butt;
    rule.paint.stroke = stroke;
    const std::array<path::PathShape, 2> shapes{box, rule};
    const std::vector<std::uint8_t> pixels = rasterize(device, path::gpu_scene_from_shapes(shapes));

    bool ok = true;
    ok =
        expect(is(at(pixels, 30, 15), 255, 0, 0), "the filled rectangle's interior is its color") &&
        ok;
    ok =
        expect(is(at(pixels, 10, 6), 255, 0, 0) && is(at(pixels, 59, 25), 255, 0, 0),
               "the rectangle's corner pixels are fully covered (a pixel-aligned edge is exact)") &&
        ok;
    ok = expect(is(at(pixels, 9, 15), 0, 0, 0) && is(at(pixels, 60, 15), 0, 0, 0) &&
                    is(at(pixels, 30, 5), 0, 0, 0) && is(at(pixels, 30, 26), 0, 0, 0),
                "pixels just outside the rectangle are untouched") &&
         ok;
    ok = expect(is(at(pixels, 50, 33), 0, 0, 255) && is(at(pixels, 50, 34), 0, 0, 255),
                "the butt-capped stroke's interior rows are its color") &&
         ok;
    ok = expect(is(at(pixels, 50, 31), 0, 0, 0) && is(at(pixels, 50, 36), 0, 0, 0),
                "rows beside the stroke are untouched") &&
         ok;
    ok = expect(is(at(pixels, 9, 34), 0, 0, 0) && is(at(pixels, 90, 34), 0, 0, 0),
                "a butt cap ends at the endpoints") &&
         ok;
    // The last texel of a row lies past the 400 bytes of pixels, in the padded pitch: the far
    // column must still be the right pixel, not the start of the next row.
    ok = expect(is(at(pixels, kWidth - 1, 10), 0, 0, 0) && is(at(pixels, 0, 11), 0, 0, 0),
                "the padded row pitch does not shift pixels") &&
         ok;
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
        std::cout << "path_raster_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!device->supports_compute() || !device->supports_buffer_texture_copy()) {
        std::cout << "path_raster_gpu: skipped (no compute or buffer-to-texture copy)\n";
        return 0;
    }
    try {
        if (!run(*device)) {
            std::cerr << "path_raster_gpu: FAILED\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "path_raster_gpu: unexpected exception: " << error.what() << "\n";
        return 3;
    }
    std::cout << "path_raster_gpu: ok\n";
    return 0;
}
