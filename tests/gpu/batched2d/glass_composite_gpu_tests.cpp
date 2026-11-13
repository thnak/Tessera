// SPDX-License-Identifier: MIT
//
// The first real glass pixels: a Vulkan/lavapipe pixel test for the RHI 1.3 sampled render target
// (issue #254). The frame draws a red|blue backdrop, breaks the pass, copies the backdrop into a
// pooled target, runs the horizontal `glass_blur` pass into a second pooled target sampling that
// copy, then resumes the frame and composites the blurred result through the vertical pass.
//
// What this pins that the null structural test cannot:
//  - a pooled target is readable as a texture after a region copy (layout: TRANSFER_SRC ->
//    SHADER_READ_ONLY) and again after being written by a pass;
//  - the separable Gaussian is the one `glass_blur.cpp` computes (the expected pixels come from
//    glass_kernel_taps plus a bilinear clamp-to-edge sample, not from a hand-written number);
//  - a sampled target can be read back afterwards (SHADER_READ_ONLY -> TRANSFER_SRC), so sampling
//    and readback alternate on one target without a caller-visible barrier.
//
// Deterministic UNORM target, exact geometry, byte comparison with a 1/255 rounding tolerance; the
// test skips cleanly when no Vulkan device is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassKernel;
using tessera::rendering::gpu::GlassPushConstants;
using tessera::rendering::gpu::GlassTap;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::Offset2D;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kSplit = 32; // x < kSplit is red, x >= kSplit is blue
constexpr float kSigma = 2.0f;       // blur-target texels
constexpr int kTolerance = 2;        // out of 255, for float32 vs double rounding
constexpr std::uint32_t kFullscreenVertices = 3;

constexpr Color kBlack = rgba8(0, 0, 0);
constexpr Color kRed = rgba8(255, 0, 0);
constexpr Color kBlue = rgba8(0, 0, 255);

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_composite_gpu: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.clear_color = kBlack;
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    // The glass blur's tap pairing needs linear filtering and a border-safe edge mode; these are
    // the RHI defaults and the state the sampled view is created with.
    desc.filter = tessera::rendering::rhi::Filter::Linear;
    desc.address = tessera::rendering::rhi::AddressMode::ClampToEdge;
    return desc;
}

struct Rgb {
    float r = 0;
    float g = 0;
    float b = 0;
};

// One backdrop texel of the frame the test draws: the left half is red, the right half blue.
Rgb backdrop_texel(std::uint32_t x) {
    return x < kSplit ? Rgb{1.0f, 0.0f, 0.0f} : Rgb{0.0f, 0.0f, 1.0f};
}

// Bilinear sample of the backdrop at a fractional pixel position, with clamp-to-edge addressing:
// the CPU twin of the sampler state the H pass reads the copied backdrop with.
Rgb sample_backdrop(float position) {
    const float clamped = std::clamp(position, 0.0f, static_cast<float>(kSize) - 1.0f);
    const auto low = static_cast<std::uint32_t>(std::floor(clamped));
    const std::uint32_t high = std::min(low + 1, kSize - 1);
    const float t = clamped - static_cast<float>(low);
    const Rgb a = backdrop_texel(low);
    const Rgb b = backdrop_texel(high);
    return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// The horizontal Gaussian result at pixel x, from the same kernel the renderer records: the shader
// samples at `x + tap.offset` (its uv is the pixel's normalized center plus the tap step) and sums
// the normalized weights, which is exactly what glass_kernel_taps() returns.
Rgb expected_blur_horizontal(std::uint32_t x) {
    const GlassKernel kernel = tessera::rendering::gpu::make_glass_kernel(kSigma);
    std::array<GlassTap, 64> taps{};
    const std::size_t tap_count =
        tessera::rendering::gpu::glass_kernel_taps(kernel, std::span<GlassTap>(taps));
    Rgb sum;
    for (std::size_t tap = 0; tap < tap_count; ++tap) {
        const Rgb sample = sample_backdrop(static_cast<float>(x) + taps[tap].offset);
        sum.r += sample.r * taps[tap].weight;
        sum.g += sample.g * taps[tap].weight;
        sum.b += sample.b * taps[tap].weight;
    }
    return sum;
}

// Horizontal pass: blur along x into a same-sized blur target.
GlassPushConstants horizontal_constants() {
    GlassPushConstants pc;
    pc.texel_step_x = 1.0f / static_cast<float>(kSize);
    pc.texel_step_y = pc.texel_step_x;
    pc.direction_x = 1.0f;
    pc.direction_y = 0.0f;
    pc.sigma = kSigma;
    pc.radius = static_cast<float>(tessera::rendering::gpu::make_glass_kernel(kSigma).radius);
    pc.region_origin_x = 0.0f;
    pc.region_origin_y = 0.0f;
    pc.region_size_x = static_cast<float>(kSize);
    pc.region_size_y = static_cast<float>(kSize);
    pc.box_x = 0.0f;
    pc.box_y = 0.0f;
    pc.box_width = static_cast<float>(kSize);
    pc.box_height = static_cast<float>(kSize);
    pc.mode = 0.0f;
    return pc;
}

// Vertical pass + composite: the backdrop is constant along y, so this is the identity on the
// horizontal result; factors are neutral and the rounded box covers the whole target.
GlassPushConstants vertical_constants() {
    GlassPushConstants pc;
    pc.texel_step_x = 1.0f / static_cast<float>(kSize);
    pc.texel_step_y = pc.texel_step_x;
    pc.direction_x = 0.0f;
    pc.direction_y = 1.0f;
    pc.sigma = kSigma;
    pc.radius = static_cast<float>(tessera::rendering::gpu::make_glass_kernel(kSigma).radius);
    pc.region_origin_x = 0.0f;
    pc.region_origin_y = 0.0f;
    pc.region_size_x = static_cast<float>(kSize);
    pc.region_size_y = static_cast<float>(kSize);
    pc.box_x = 0.0f;
    pc.box_y = 0.0f;
    pc.box_width = static_cast<float>(kSize);
    pc.box_height = static_cast<float>(kSize);
    pc.saturate = 1.0f;
    pc.brightness = 1.0f;
    pc.contrast = 1.0f;
    pc.mode = 1.0f;
    return pc;
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const Extent2D extent = target.extent();
    const std::size_t bytes =
        static_cast<std::size_t>(extent.width) * extent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool close_to(int actual, float expected, const char* what, std::uint32_t x) {
    const int target = static_cast<int>(std::lround(expected * 255.0f));
    if (std::abs(actual - target) <= kTolerance) {
        return true;
    }
    std::cerr << "glass_composite_gpu: " << what << " at x=" << x << ": got " << actual
              << ", expected " << target << "\n";
    return false;
}

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "glass_composite_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    const RenderTargetDesc desc = target_desc();
    auto frame = device->create_render_target(desc);
    auto batched2d_pipeline =
        device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    auto glass_pipeline =
        device->create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc());

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Batched2DRenderer renderer(*device, *batched2d_pipeline, *white, config);

    // --- Draw the backdrop, break, copy, blur horizontally, composite -------------------------
    auto commands = device->create_command_list();
    renderer.begin(*commands, *frame, desc);
    renderer.push_rect({0.0f, 0.0f, static_cast<float>(kSplit), static_cast<float>(kSize)}, kRed);
    renderer.push_rect({static_cast<float>(kSplit), 0.0f, static_cast<float>(kSize - kSplit),
                        static_cast<float>(kSize)},
                       kBlue);
    renderer.flush();
    renderer.break_pass();

    RenderTarget* backdrop = renderer.acquire_offscreen_target({kSize, kSize});
    RenderTarget* blur = renderer.acquire_offscreen_target({kSize, kSize});
    if (!expect(backdrop != nullptr && blur != nullptr,
                "the offscreen pool hands out both blur targets")) {
        return 1;
    }
    commands->copy_target_region(*frame, Scissor{0, 0, kSize, kSize}, *backdrop, Offset2D{});

    const GlassPushConstants horizontal = horizontal_constants();
    commands->bind_pipeline(*glass_pipeline);
    commands->bind_sampled_target(0, *backdrop);
    commands->begin_render_pass(*blur, desc);
    commands->set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
    commands->set_scissor(Scissor{0, 0, kSize, kSize});
    commands->push_constants(&horizontal, sizeof(horizontal));
    commands->draw(kFullscreenVertices);
    commands->end_render_pass();

    const GlassPushConstants vertical = vertical_constants();
    commands->bind_sampled_target(0, *blur);
    renderer.resume_pass();
    // resume_pass() rebinds the batched 2D pipeline: the sampling pipeline is rebound for the
    // composite draw, exactly as a renderer that paints the node after the break will.
    commands->bind_pipeline(*glass_pipeline);
    commands->set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
    commands->set_scissor(Scissor{0, 0, kSize, kSize});
    commands->push_constants(&vertical, sizeof(vertical));
    commands->draw(kFullscreenVertices);
    renderer.end();
    commands->submit();
    device->wait_idle();

    // --- The copied backdrop holds the frame's pixels, unblurred -------------------------------
    const std::vector<std::uint8_t> copied = read_target(*device, *backdrop);
    bool ok = expect(pixel_at(copied, 4, 32)[0] == 255 && pixel_at(copied, 4, 32)[2] == 0,
                     "the backdrop copy holds the source's red half") &&
              expect(pixel_at(copied, 60, 32)[2] == 255 && pixel_at(copied, 60, 32)[0] == 0,
                     "the backdrop copy holds the source's blue half");

    // --- The blur target holds the separable Gaussian, sampled from the copy -------------------
    // Readback after a sampled bind also proves the SHADER_READ_ONLY -> TRANSFER_SRC transition.
    const std::vector<std::uint8_t> blurred = read_target(*device, *blur);
    for (std::uint32_t x = 0; x < kSize; ++x) {
        const Rgb expected = expected_blur_horizontal(x);
        const std::array<std::uint8_t, 4> pixel = pixel_at(blurred, x, 32);
        ok = close_to(pixel[0], expected.r, "blurred red", x) &&
             close_to(pixel[1], expected.g, "blurred green", x) &&
             close_to(pixel[2], expected.b, "blurred blue", x) && ok;
    }
    ok = expect(pixel_at(blurred, static_cast<std::uint32_t>(kSplit) - 16, 32)[0] == 255,
                "a pixel far from the edge stays fully red") &&
         ok;
    ok = expect(pixel_at(blurred, static_cast<std::uint32_t>(kSplit) + 16, 32)[2] == 255,
                "a pixel far from the edge stays fully blue") &&
         ok;

    // --- The composite wrote the blurred pixels into the frame target -------------------------
    const std::vector<std::uint8_t> composed = read_target(*device, *frame);
    for (std::uint32_t x = 0; x < kSize; ++x) {
        const Rgb expected = expected_blur_horizontal(x);
        const std::array<std::uint8_t, 4> pixel = pixel_at(composed, x, 48);
        ok = close_to(pixel[0], expected.r, "composited red", x) &&
             close_to(pixel[1], expected.g, "composited green", x) &&
             close_to(pixel[2], expected.b, "composited blue", x) && ok;
    }
    const std::array<std::uint8_t, 4> edge =
        pixel_at(composed, static_cast<std::uint32_t>(kSplit), 48);
    ok = expect(edge[0] > 40 && edge[0] < 215 && edge[2] > 40 && edge[2] < 215,
                "the composite blends the two halves at the split (the glass color)") &&
         ok;

    // Validation-clean: the layout transitions this test drives (copy -> shader-read -> transfer,
    // twice) must not produce a single barrier, layout or lifetime message. This is the half of the
    // proof a pixel cannot give.
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;

    if (!ok) {
        std::cerr << "glass_composite_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "glass_composite_gpu: ok\n";
    return 0;
}
