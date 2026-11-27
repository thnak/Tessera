// SPDX-License-Identifier: MIT
//
// The offscreen layer pool on a real device (issue #617, docs/design/gpu/offscreen-layers.md
// section 5e): N sibling masked layers all mask correctly, in pixels, while the pool holds two
// targets.
//
// The null-backend twin (layer_pool_null_tests.cpp) proves the recorded stream is well ordered;
// this proves the device honours it, which is the part a recycled target puts at risk: a layer or
// mask target cleared again inside the frame (or in the next frame at any ring depth) must wait for
// the passes and sampled reads that used it, and the run must be validation-clean (synchronization
// validation included), because a missing barrier shows up as corrupted pixels only sometimes.
//
// Evidence, per N in {6, 12} and per frame ring depth in {1, 2, 8}, over three frames each:
//  - the left half of every card shows its own colour and the right half shows the black frame
//    (every mask applied, none leaked into a neighbour), and the gaps between cards stay black;
//  - three blurred-then-masked cards in a second row keep their colour at the centre of the masked
//    half and nothing on the other half (the filter scratch target is recycled into the mask);
//  - all three frames are byte-identical, so a recycled target never carries the previous frame;
//  - peak live targets are two for the masked row, three with the filtered row's scratch, never
//  over
//    the documented bound, and nothing was refused;
//  - no validation message.
//
// Skips cleanly when the machine has no Vulkan device.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <array>
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
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;

constexpr std::uint32_t kWidth = 128;
constexpr std::uint32_t kHeight = 96;
constexpr int kTolerance = 3; // out of 255

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_pool_gpu: " << message << "\n";
    }
    return condition;
}

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
    std::unique_ptr<rhi::Pipeline> mask;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<int, 3> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                            std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2]};
}

bool near(const std::array<int, 3>& actual, const std::array<int, 3>& want, const char* what,
          std::uint32_t x, std::uint32_t y) {
    if (std::abs(actual[0] - want[0]) <= kTolerance &&
        std::abs(actual[1] - want[1]) <= kTolerance &&
        std::abs(actual[2] - want[2]) <= kTolerance) {
        return true;
    }
    std::cerr << "layer_pool_gpu: " << what << " at (" << x << "," << y << "): got (" << actual[0]
              << "," << actual[1] << "," << actual[2] << "), expected (" << want[0] << ","
              << want[1] << "," << want[2] << ")\n";
    return false;
}

// Card i of the first row: x = 4 + 10 i, 8 wide, y 8..56. A distinct colour per card, so a mask or
// layer that landed on a neighbour shows.
std::array<int, 3> card_colour(int index) {
    return {255 - 12 * index, 20 + 18 * index, 40 + 10 * index};
}

rhi::Color to_color(const std::array<int, 3>& rgb) {
    return rhi::rgba8(static_cast<std::uint8_t>(rgb[0]), static_cast<std::uint8_t>(rgb[1]),
                      static_cast<std::uint8_t>(rgb[2]));
}

void masked_card(Batched2DRenderer& renderer, const rhi::Rect& box, const rhi::Color& colour,
                 const LayerFilterChain& filter = {}) {
    renderer.push_layer(box, 1.0f, filter);
    renderer.push_rect(box, colour);
    if (renderer.begin_layer_mask()) {
        renderer.push_rect({box.x, box.y, box.width * 0.5f, box.height},
                           rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
        renderer.end_layer_mask();
    }
    renderer.pop_layer();
}

struct Result {
    std::vector<std::uint8_t> pixels;
    rhi::RenderTarget* unused = nullptr;
};

bool scene(Harness& harness, int cards, std::uint32_t ring) {
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    config.frames_in_flight = ring;
    const GlassPipelines pipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()};
    Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                               pipelines);
    LayerFilterChain blur;
    blur.count = 1;
    blur.ops[0].kind = LayerFilterOp::Kind::Blur;
    blur.ops[0].sigma = 1.0f;

    std::vector<std::uint8_t> first;
    bool ok = true;
    for (int frame = 0; frame < 3; ++frame) {
        auto target = harness.device->create_render_target(target_desc());
        auto commands = harness.device->create_command_list();
        renderer.begin(*commands, *target, target_desc());
        for (int index = 0; index < cards; ++index) {
            masked_card(renderer, {static_cast<float>(4 + index * 10), 8.0f, 8.0f, 48.0f},
                        to_color(card_colour(index)));
        }
        for (int index = 0; index < 3; ++index) {
            masked_card(renderer, {static_cast<float>(8 + index * 40), 62.0f, 24.0f, 24.0f},
                        to_color(card_colour(index)), blur);
        }
        renderer.end();
        commands->submit();
        const std::vector<std::uint8_t> pixels = read_target(*harness.device, *target);

        const auto stats = renderer.offscreen_stats();
        const auto n = static_cast<std::uint32_t>(cards) + 3;
        ok = expect(renderer.layers_isolated() == n && renderer.layers_flat() == 0,
                    "every card isolates") &&
             expect(renderer.layer_masks_applied() == n && renderer.layer_masks_unsupported() == 0,
                    "every card is masked") &&
             expect(renderer.layer_filters_applied() == 3 &&
                        renderer.layer_filters_unsupported() == 0,
                    "every blur ran") &&
             expect(stats.exhausted == 0 && stats.exhausted_total == 0, "nothing was refused") &&
             expect(stats.peak_live == 2 && stats.targets == 2,
                    "a masked or blurred card needs two live targets") &&
             ok;
        for (int index = 0; index < cards; ++index) {
            const auto x = static_cast<std::uint32_t>(4 + index * 10);
            ok = near(pixel_at(pixels, x + 2, 32), card_colour(index), "masked half", x + 2, 32) &&
                 near(pixel_at(pixels, x + 6, 32), {0, 0, 0}, "the mask's hole", x + 6, 32) &&
                 near(pixel_at(pixels, x + 9, 32), {0, 0, 0}, "the gap between cards", x + 9, 32) &&
                 ok;
        }
        for (int index = 0; index < 3; ++index) {
            const auto x = static_cast<std::uint32_t>(8 + index * 40);
            ok = near(pixel_at(pixels, x + 6, 74), card_colour(index), "blurred masked half", x + 6,
                      74) &&
                 near(pixel_at(pixels, x + 20, 74), {0, 0, 0}, "blurred card's hole", x + 20, 74) &&
                 ok;
        }
        if (frame == 0) {
            first = pixels;
        } else {
            ok = expect(pixels == first, "a later frame is byte-identical to the first") && ok;
        }
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
        std::cout << "layer_pool_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    harness.pipeline = harness.device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(format));
    harness.blur = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    harness.composite = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
    harness.mask =
        harness.device->create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc(
            format, rhi::BlendMode::DestinationIn));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = true;
    for (const int cards : {6, 12}) {
        for (const std::uint32_t ring : {1u, 2u, 8u}) {
            ok = scene(harness, cards, ring) && ok;
        }
    }

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_pool_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_pool_gpu: ok\n";
    return 0;
}
