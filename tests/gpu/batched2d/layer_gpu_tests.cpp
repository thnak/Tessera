// SPDX-License-Identifier: MIT
//
// Isolated layers (issue #227, docs/design/gpu/offscreen-layers.md section 5.2) on a real device:
// the pixels the layer path must produce, with the flat fallback as the contrast.
//
// Evidence:
//  - isolation: two overlapping opaque rects inside a 0.5 layer composite to the TOP rect at half
//    strength (the group fades as one image), where the flat rule shows the lower rect through it;
//  - premultiplied accumulation: a half-transparent white rect in a 0.5 layer over black lands at
//    0.25, so the layer's colour is un-premultiplied once and opacity applied once;
//  - nesting multiplies (0.5 inside 0.5 is 0.25) and a layer's scissor bounds its composite;
//  - nothing outside the layer's box changes;
//  - the run is validation-clean (the layer pass, the sampled bind and the reopened frame pass).
//
// Skips cleanly when the machine has no Vulkan device.

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
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 2; // out of 255

constexpr rhi::Color kRed = rhi::rgba8(255, 0, 0);
constexpr rhi::Color kBlue = rhi::rgba8(0, 0, 255);

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_gpu: " << message << "\n";
    }
    return condition;
}

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
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

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<int, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                            std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool near(const std::array<int, 4>& actual, int r, int g, int b, const char* what, std::uint32_t x,
          std::uint32_t y) {
    if (std::abs(actual[0] - r) <= kTolerance && std::abs(actual[1] - g) <= kTolerance &&
        std::abs(actual[2] - b) <= kTolerance) {
        return true;
    }
    std::cerr << "layer_gpu: " << what << " at (" << x << "," << y << "): got (" << actual[0] << ","
              << actual[1] << "," << actual[2] << "), expected (" << r << "," << g << "," << b
              << ")\n";
    return false;
}

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t isolated = 0;
    std::uint32_t flat = 0;
};

// Draws `scene` between begin() and end() and reads the frame back. `with_layers` selects whether
// the renderer owns the composite pipeline (isolated layers) or not (the flat fallback).
template<typename F> Frame render(Harness& harness, bool with_layers, F&& scene) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        const GlassPipelines pipelines =
            with_layers ? GlassPipelines{harness.blur.get(), harness.composite.get()}
                        : GlassPipelines{};
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   pipelines);
        renderer.begin(*commands, *target, target_desc());
        scene(renderer);
        renderer.end();
        commands->submit();
        frame.isolated = renderer.layers_isolated();
        frame.flat = renderer.layers_flat();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

// Two overlapping opaque rects in one 0.5 layer: red (8..40) under blue (24..56), box 8..56.
void overlapping_pair(Batched2DRenderer& renderer) {
    renderer.push_layer({8.0f, 8.0f, 48.0f, 48.0f}, 0.5f);
    renderer.push_rect({8.0f, 8.0f, 32.0f, 32.0f}, kRed);
    renderer.push_rect({24.0f, 24.0f, 32.0f, 32.0f}, kBlue);
    renderer.pop_layer();
}

bool isolation(Harness& harness) {
    const Frame frame = render(harness, true, overlapping_pair);
    bool ok = expect(frame.isolated == 1 && frame.flat == 0, "the layer was isolated");
    ok =
        near(pixel_at(frame.pixels, 12, 12), 128, 0, 0, "red only, at half strength", 12, 12) && ok;
    ok = near(pixel_at(frame.pixels, 50, 50), 0, 0, 128, "blue only, at half strength", 50, 50) &&
         ok;
    // The overlap shows the top rect alone: the group is one image faded once.
    ok = near(pixel_at(frame.pixels, 32, 32), 0, 0, 128, "the overlap is the top rect only", 32,
              32) &&
         ok;
    ok = near(pixel_at(frame.pixels, 2, 2), 0, 0, 0, "outside the box is untouched", 2, 2) && ok;
    ok =
        near(pixel_at(frame.pixels, 60, 60), 0, 0, 0, "outside the box is untouched", 60, 60) && ok;
    ok = expect(pixel_at(frame.pixels, 32, 32)[3] == 255, "the frame stays opaque") && ok;
    return ok;
}

bool flat_fallback_contrast(Harness& harness) {
    const Frame frame = render(harness, false, overlapping_pair);
    bool ok = expect(frame.isolated == 0 && frame.flat == 1, "no composite pipeline paints flat");
    ok = near(pixel_at(frame.pixels, 12, 12), 128, 0, 0, "flat red at half strength", 12, 12) && ok;
    // The non-isolated rule: the red under the blue still shows through at half strength twice.
    ok = near(pixel_at(frame.pixels, 32, 32), 64, 0, 128, "flat overlap shows the lower rect", 32,
              32) &&
         ok;
    return ok;
}

bool premultiplied_accumulation(Harness& harness) {
    const Frame frame = render(harness, true, [](Batched2DRenderer& renderer) {
        renderer.push_layer({8.0f, 8.0f, 48.0f, 48.0f}, 0.5f);
        renderer.push_rect({8.0f, 8.0f, 48.0f, 48.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 0.5f});
        renderer.pop_layer();
    });
    // 0.5 white inside, then 0.5 for the group, over black: 0.25.
    return near(pixel_at(frame.pixels, 32, 32), 64, 64, 64,
                "half-transparent white in a half layer", 32, 32);
}

bool nesting_multiplies(Harness& harness) {
    const Frame frame = render(harness, true, [](Batched2DRenderer& renderer) {
        renderer.push_layer({8.0f, 8.0f, 48.0f, 48.0f}, 0.5f);
        renderer.push_layer({8.0f, 8.0f, 48.0f, 48.0f}, 0.5f);
        renderer.push_rect({8.0f, 8.0f, 48.0f, 48.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
        renderer.pop_layer();
        renderer.pop_layer();
    });
    return expect(frame.isolated == 2, "both layers were isolated") &&
           near(pixel_at(frame.pixels, 32, 32), 64, 64, 64, "0.5 inside 0.5 is 0.25", 32, 32);
}

bool scissor_bounds_the_composite(Harness& harness) {
    const Frame frame = render(harness, true, [](Batched2DRenderer& renderer) {
        renderer.push_scissor(rhi::Scissor{0, 0, 32, 64}); // the left half only
        renderer.push_layer({8.0f, 8.0f, 48.0f, 48.0f}, 1.0f);
        renderer.push_rect({8.0f, 8.0f, 48.0f, 48.0f}, kRed);
        renderer.pop_layer();
        renderer.pop_scissor();
    });
    return near(pixel_at(frame.pixels, 20, 32), 255, 0, 0, "inside the scissor", 20, 32) &&
           near(pixel_at(frame.pixels, 44, 32), 0, 0, 0, "outside the scissor", 44, 32);
}

bool frames_repeat(Harness& harness) {
    // Pooled targets recycle across frames; a second frame must not inherit the first's pixels.
    bool ok = true;
    for (int frame_index = 0; frame_index < 4; ++frame_index) {
        const Frame frame = render(harness, true, overlapping_pair);
        ok = near(pixel_at(frame.pixels, 32, 32), 0, 0, 128, "a repeated frame is the same", 32,
                  32) &&
             ok;
    }
    return ok;
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
        std::cout << "layer_gpu: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    harness.pipeline = harness.device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(format));
    harness.blur = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    harness.composite = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = isolation(harness);
    ok = flat_fallback_contrast(harness) && ok;
    ok = premultiplied_accumulation(harness) && ok;
    ok = nesting_multiplies(harness) && ok;
    ok = scissor_bounds_the_composite(harness) && ok;
    ok = frames_repeat(harness) && ok;

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_gpu: ok\n";
    return 0;
}
