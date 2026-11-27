// SPDX-License-Identifier: MIT
//
// `mask-composite` (issue #617, docs/design/gpu/offscreen-layers.md section 5.2f) on a real device:
// the pixels the combine step must produce. A red box layer is masked by two mask layers, B at the
// bottom (x 8..32) and A on top (x 20..44), and A's operator decides how it meets B:
//   subtract: A where B is empty, intersect: A where B is, exclude: where exactly one of them is.
// Opaque A is checked in the top half of the box, half-alpha A in the bottom half (so the
// fractional factors `1 - dst.a` and `1 - src.a` are exercised), and a renderer without the
// operator's pipeline drops the layer and counts it. Skips cleanly without a Vulkan device.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::gpu::MaskCompositeOp;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 2;
constexpr rhi::Rect kBox{8.0f, 8.0f, 48.0f, 48.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_mask_composite_gpu: " << message << "\n";
    }
    return condition;
}

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
    std::unique_ptr<rhi::Pipeline> mask;
    std::unique_ptr<rhi::Pipeline> mask_out;
    std::unique_ptr<rhi::Pipeline> mask_xor;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t unsupported = 0;
    bool began = false;
};

int red_at(const Frame& frame, std::uint32_t x, std::uint32_t y) {
    return frame.pixels[(static_cast<std::size_t>(y) * kSize + x) * 4];
}

// B (opaque) at the bottom, then A on top with `op`: opaque in the top half, `half_alpha` in the
// bottom half. `with_pipelines` false leaves the two new pipelines out.
Frame render(Harness& harness, MaskCompositeOp op, bool with_pipelines) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        const GlassPipelines pipelines{harness.blur.get(), harness.composite.get(),
                                       harness.mask.get(),
                                       with_pipelines ? harness.mask_out.get() : nullptr,
                                       with_pipelines ? harness.mask_xor.get() : nullptr};
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   pipelines);
        renderer.begin(*commands, *target, target_desc());
        renderer.push_layer(kBox, 1.0f);
        renderer.push_rect(kBox, rhi::rgba8(255, 0, 0));
        if (renderer.begin_layer_mask()) {
            renderer.push_rect({8.0f, 8.0f, 24.0f, 48.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
            frame.began = renderer.begin_mask_layer(op);
            if (frame.began) {
                renderer.push_rect({20.0f, 8.0f, 24.0f, 24.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
                renderer.push_rect({20.0f, 32.0f, 24.0f, 24.0f},
                                   rhi::Color{1.0f, 1.0f, 1.0f, 0.5f});
                renderer.end_mask_layer();
            }
            renderer.end_layer_mask();
        }
        renderer.pop_layer();
        renderer.end();
        commands->submit();
        frame.unsupported = renderer.layer_masks_unsupported();
    }
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = harness.device->create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = harness.device->create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    frame.pixels.resize(bytes);
    readback->read(frame.pixels.data(), frame.pixels.size());
    return frame;
}

bool near(int actual, int expected, const char* what, std::uint32_t x, std::uint32_t y) {
    if (std::abs(actual - expected) <= kTolerance) {
        return true;
    }
    std::cerr << "layer_mask_composite_gpu: " << what << " at (" << x << "," << y << "): got "
              << actual << ", expected " << expected << "\n";
    return false;
}

// Columns: x 12 is B only, x 26 is both, x 38 is A only, x 50 is neither. Rows: y 20 opaque A,
// y 44 half-alpha A.
bool check(Harness& harness, MaskCompositeOp op, const std::array<int, 4>& opaque,
           const std::array<int, 4>& half, const char* name) {
    const Frame frame = render(harness, op, true);
    bool ok = expect(frame.began && frame.unsupported == 0, name);
    const std::array<std::uint32_t, 4> columns = {12, 26, 38, 50};
    for (std::size_t i = 0; i < columns.size(); ++i) {
        ok = near(red_at(frame, columns[i], 20), opaque[i], name, columns[i], 20) && ok;
        ok = near(red_at(frame, columns[i], 44), half[i], name, columns[i], 44) && ok;
    }
    return ok;
}

bool operators(Harness& harness) {
    // red is 255 * mask alpha: 255 / 128 / 0.
    bool ok = check(harness, MaskCompositeOp::Subtract, {0, 0, 255, 0}, {0, 0, 128, 0}, "subtract");
    ok = check(harness, MaskCompositeOp::Intersect, {0, 255, 0, 0}, {0, 128, 0, 0}, "intersect") &&
         ok;
    ok =
        check(harness, MaskCompositeOp::Exclude, {255, 0, 255, 0}, {255, 128, 128, 0}, "exclude") &&
        ok;
    return ok;
}

bool without_the_pipelines(Harness& harness) {
    // Intersect rides the existing mask pipeline; subtract and exclude need the new ones.
    const Frame intersect = render(harness, MaskCompositeOp::Intersect, false);
    bool ok =
        expect(intersect.began && intersect.unsupported == 0, "intersect needs no new pipeline");
    const Frame subtract = render(harness, MaskCompositeOp::Subtract, false);
    ok = expect(!subtract.began && subtract.unsupported == 1, "subtract is refused and counted") &&
         ok;
    // The dropped layer's draws never ran, so the mask is B alone: red where B is.
    ok = near(red_at(subtract, 12, 20), 255, "B alone", 12, 20) && ok;
    ok = near(red_at(subtract, 38, 20), 0, "A was dropped", 38, 20) && ok;
    const Frame exclude = render(harness, MaskCompositeOp::Exclude, false);
    return expect(!exclude.began && exclude.unsupported == 1, "exclude is refused and counted") &&
           ok;
}

bool frames_repeat(Harness& harness) {
    // Targets are recycled: a second frame must not see the first one's parts.
    bool ok = check(harness, MaskCompositeOp::Subtract, {0, 0, 255, 0}, {0, 0, 128, 0}, "again 1");
    return check(harness, MaskCompositeOp::Subtract, {0, 0, 255, 0}, {0, 0, 128, 0}, "again 2") &&
           ok;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;
    Harness harness;
    try {
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "layer_mask_composite_gpu: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    const auto glass = [&](rhi::BlendMode blend) {
        return harness.device->create_pipeline(
            tessera::rendering::vulkan::glass_blur_pipeline_desc(format, blend));
    };
    harness.pipeline = harness.device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(format));
    harness.blur = glass(rhi::BlendMode::None);
    harness.composite = glass(rhi::BlendMode::Alpha);
    harness.mask = glass(rhi::BlendMode::DestinationIn);
    harness.mask_out = glass(rhi::BlendMode::SourceOut);
    harness.mask_xor = glass(rhi::BlendMode::Xor);
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = operators(harness);
    ok = without_the_pipelines(harness) && ok;
    ok = frames_repeat(harness) && ok;
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_mask_composite_gpu: failed (validation=" << validation_messages
                  << ")\n";
        return 1;
    }
    std::cout << "layer_mask_composite_gpu: ok\n";
    return 0;
}
