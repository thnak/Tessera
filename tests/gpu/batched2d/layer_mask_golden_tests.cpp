// SPDX-License-Identifier: MIT
//
// The `mask-image` lavapipe golden (issue #617): the `masks` scene of the CPU corpus
// (`cpu_wide_mask_scenes.cpp`) painted through the real paint pass, `Batched2DSink` and the
// renderer's layer mask path on a software Vulkan device, compared with the committed baseline in
// tests/golden/gpu/mask_image.png and with the CPU twin (`CpuPaintSink`) of the same scene.
//
// Three proofs:
//  - provenance: pixels are only pinned on a software rasterizer (ADR-009); a hardware device is
//    refused with a skip, so a run on a developer GPU can never write or verify these pixels;
//  - determinism: two frames are byte-identical before either is pinned;
//  - twin: the GPU and CPU renders of the scene agree within `kTwinTolerance` per channel, with
//    every layer and mask going through its own path, so a divergence in either shows here.
//
// Regenerate on a software device only: `layer_mask_golden_tests --update-goldens`. The ctest entry
// is registered on UNIX with a lavapipe manifest only, like the other lavapipe goldens.

#include "cpu_wide_scene_support.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_device_info.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_MASK_GOLDEN_PATH
#error "TESSERA_MASK_GOLDEN_PATH must point at the committed baseline PNG"
#endif

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::harness::CompareMode;
using tessera::harness::ComparePolicy;
using tessera::harness::RgbaImage;
using tessera::poc::batched2d::CpuPaintSink;
using tessera::poc::batched2d::cpu_wide_support::CpuCanvas;
using tessera::poc::batched2d::cpu_wide_support::kHeight;
using tessera::poc::batched2d::cpu_wide_support::kWidth;
using tessera::poc::batched2d::cpu_wide_support::StubStyles;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;

// The twin may differ by float-versus-byte rounding of the composite and the straight-alpha blend
// (measured on lavapipe: at most 1 of 255).
constexpr int kTwinTolerance = 2; // measured worst delta 1 on llvmpipe

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_mask_golden: " << message << "\n";
        ++failures;
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

struct Scene {
    tessera::ui::Node root{1};
    StubStyles styles;
    Scene() {
        tessera::poc::batched2d::cpu_wide_support::mask_cases().front().build(root, styles);
    }
};

std::vector<std::uint8_t> render_gpu(Harness& harness, std::uint32_t& masks_applied) {
    Scene scene;
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    // Four masked nodes need two pooled targets each; the default pool of 4 would paint the last
    // two unmasked (offscreen-layers.md section 5a), which the CPU twin never does.
    config.max_offscreen_targets = 8;
    {
        Batched2DRenderer renderer(
            *harness.device, *harness.pipeline, *harness.white, config,
            GlassPipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()});
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink);
        renderer.end();
        commands->submit();
        masks_applied = renderer.layer_masks_applied();
    }
    return read_target(*harness.device, *target);
}

std::vector<std::uint8_t> render_cpu() {
    Scene scene;
    CpuCanvas canvas(kWidth, kHeight);
    CpuPaintSink sink(canvas);
    (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink);
    return {canvas.pixels().begin(), canvas.pixels().end()};
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// The largest per-channel difference between two frames, and where. Alpha is skipped: both frames
// are opaque.
int worst_difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                     std::size_t& at) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) {
            continue;
        }
        const int delta = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (delta > worst) {
            worst = delta;
            at = i / 4;
        }
    }
    return worst;
}

} // namespace

int main(int argc, char** argv) {
    bool update_goldens = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "layer_mask_golden: unknown argument " << argv[index] << "\n";
        return 2;
    }

    Harness harness;
    try {
        rhi::DeviceDesc device_desc;
        device_desc.backend = rhi::Backend::Vulkan;
        device_desc.validation = true;
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "layer_mask_golden: skipped (no Vulkan device: " << error.what() << ")\n";
        return 0;
    }
    const auto info = tessera::rendering::vulkan::device_info(*harness.device);
    if (!info || !info->software()) {
        std::cout << "layer_mask_golden: skipped (no software Vulkan device on this host"
                  << (info ? ": " + info->describe() : std::string()) << ")\n";
        return 0;
    }
    std::cout << "layer_mask_golden: device " << info->describe() << "\n";

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

    std::uint32_t applied = 0;
    const std::vector<std::uint8_t> first = render_gpu(harness, applied);
    expect(applied == 4, "the scene's four masked nodes each ran the GPU mask step");
    std::uint32_t applied_again = 0;
    const std::vector<std::uint8_t> second = render_gpu(harness, applied_again);
    if (!expect(first == second, "two independent frames are byte-identical")) {
        std::cerr << "layer_mask_golden: the mask path is not deterministic on this device; "
                     "refusing to pin pixels\n";
        return 1;
    }

    // The twin: every pixel of the CPU render within tolerance of the GPU one.
    const std::vector<std::uint8_t> cpu = render_cpu();
    std::size_t at = 0;
    const int worst = worst_difference(first, cpu, at);
    std::cout << "layer_mask_golden: CPU twin worst channel delta " << worst << " at ("
              << at % kWidth << "," << at / kWidth << ")\n";
    expect(worst <= kTwinTolerance, "the CPU twin agrees with the GPU within the tolerance");

    expect(tessera::rendering::vulkan::validation_message_count() == 0,
           "the run produced no validation messages");

    RgbaImage frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.rgba = first;

    if (update_goldens) {
        const auto png = tessera::harness::encode_png(frame);
        if (!expect(png.has_value(), "the frame encodes as PNG") ||
            !expect(write_file(TESSERA_MASK_GOLDEN_PATH, *png), "the baseline was written")) {
            return 1;
        }
        std::cout << "layer_mask_golden: wrote " << TESSERA_MASK_GOLDEN_PATH << " from "
                  << info->describe() << "\n";
        return failures == 0 ? 0 : 1;
    }

    const std::vector<std::uint8_t> baseline_bytes = read_file(TESSERA_MASK_GOLDEN_PATH);
    if (!expect(!baseline_bytes.empty(), "the committed baseline exists")) {
        std::cerr
            << "layer_mask_golden: regenerate it with --update-goldens on a software device\n";
        return 1;
    }
    const auto baseline = tessera::harness::decode_png(baseline_bytes);
    if (!expect(baseline.has_value(), "the committed baseline decodes as PNG") ||
        !expect(baseline->width == kWidth && baseline->height == kHeight,
                "the baseline has the scene's extent")) {
        return 1;
    }
    const ComparePolicy policy{0, 0.0};
    const tessera::harness::CompareResult result =
        tessera::harness::compare_images(CompareMode::Golden, frame, &*baseline, policy);
    if (!expect(result.passed(), "the masked scene is bit-identical to the baseline")) {
        std::cerr << "layer_mask_golden: pixels=" << result.diff.pixels
                  << " mismatched=" << result.diff.mismatched
                  << " max_delta=" << result.diff.max_channel_delta << "\n";
    }
    if (failures != 0) {
        std::cerr << "layer_mask_golden: failed\n";
        return 1;
    }
    std::cout << "layer_mask_golden: ok\n";
    return 0;
}
