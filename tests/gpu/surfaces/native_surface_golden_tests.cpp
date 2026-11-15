// SPDX-License-Identifier: MIT
//
// AF6 acceptance (issue #178, ADR-050 section 7): "a lavapipe golden of a composited native
// surface".
//
// This renders one surface the way a host does - `SurfaceRenderer` over a borrowed
// `hal::NativeSurface`, the scene drawn through `Batched2DRenderer` into the surface's own target,
// then composited into the destination - reads the destination back, and compares it against the
// committed baseline.
//
// Two proofs, kept separate on purpose:
//
//  * **provenance**: pixels are only pinned when the Vulkan device is a software rasterizer
//    (`VkPhysicalDeviceProperties::deviceType == CPU`, or a driver naming itself llvmpipe /
//    lavapipe / SwiftShader). A hardware device is refused with the device string in the message -
//    the rule `scripts/ci/scene_frame_parity.py --write-goldens` applies (ADR-009), because a
//    golden from the wrong renderer is worse than no golden. A host with no software device prints
//    the documented skip line and exits 0.
//  * **determinism**: the frame is rendered twice in one process and the two reads must be
//    byte-identical before either is compared with the baseline, so a flaky surface path cannot
//    pass as a golden. `--update-goldens` rewrites the baseline only after that check.
//
// Regenerate the baseline on a software device only (both variables are needed, and the manifest's
// directory must be on PATH so the loader can find the DLL it names):
//
//     native_surface_golden_tests --update-goldens
//
// The ctest entry passes neither the flag nor an ICD: it is registered on UNIX only (the lavapipe
// lane) and skips everywhere else, so a Windows hardware run can never write these pixels.

#include "tessera/hal/surface.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/surface_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_device_info.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_SURFACE_GOLDEN_PATH
#error "TESSERA_SURFACE_GOLDEN_PATH must point at the committed baseline PNG"
#endif

namespace {

using tessera::hal::NativeSurface;
using tessera::hal::SurfaceDesc;
using tessera::hal::SurfaceLifetime;
using tessera::hal::WindowSystem;
using tessera::harness::CompareMode;
using tessera::harness::ComparePolicy;
using tessera::harness::RgbaImage;
using tessera::rendering::SurfaceCompositeStatus;
using tessera::rendering::SurfaceFrame;
using tessera::rendering::SurfaceRenderer;
using tessera::rendering::SurfaceRendererConfig;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;

constexpr Extent2D kExtent{64, 64};

int failures = 0;
bool update_goldens = false;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "native_surface_golden: " << message << "\n";
        ++failures;
    }
    return condition;
}

SurfaceDesc surface_desc() {
    SurfaceDesc desc;
    desc.width_px = kExtent.width;
    desc.height_px = kExtent.height;
    desc.dpi_scale = 1.0f;
    // Headless: no platform target, which is the offscreen half of the same path the window runs.
    // The composite destination stands in for the swapchain image.
    desc.window_system = WindowSystem::NoWindow;
    return desc;
}

// Four quadrants in four unmistakable colours plus a centre square and a corner marker, so a
// channel swap, a flipped row, a shifted composite or an unfiltered resize all show up instead of
// averaging out into a plausible image.
void paint_scene(Batched2DRenderer& batched) {
    batched.push_rect({0, 0, 32, 32}, rgba8(220, 40, 40, 255));     // top-left, red
    batched.push_rect({32, 0, 32, 32}, rgba8(40, 200, 60, 255));    // top-right, green
    batched.push_rect({0, 32, 32, 32}, rgba8(50, 70, 210, 255));    // bottom-left, blue
    batched.push_rect({32, 32, 32, 32}, rgba8(240, 210, 60, 255));  // bottom-right, yellow
    batched.push_rect({24, 24, 16, 16}, rgba8(250, 250, 250, 255)); // centre, white
    batched.push_rect({4, 4, 8, 8}, rgba8(0, 0, 0, 255));           // corner marker, black
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    std::unique_ptr<tessera::rendering::rhi::Buffer> readback =
        device.create_buffer({bytes, BufferUsage::Readback});
    std::unique_ptr<tessera::rendering::rhi::CommandList> commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    // Readback is the one place a synchronous submit is correct (the RHI documents `submit()` for
    // readback and tests); the frame itself was submitted asynchronously.
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

// One full surface frame: begin, draw, composite, drain, read the destination back. Empty when any
// step reports a state instead of a frame, which the caller asserts on.
std::vector<std::uint8_t> render_surface_frame(Device& device) {
    SurfaceLifetime owner;
    const NativeSurface surface = NativeSurface::borrow(owner, surface_desc(), {}, 5);

    SurfaceRendererConfig config;
    config.clear_color = rgba8(17, 24, 39, 255);
    SurfaceRenderer renderer(device, surface, config);
    if (!renderer.valid()) {
        return {};
    }

    RenderTargetDesc destination_desc;
    destination_desc.extent = kExtent;
    destination_desc.format = config.format;
    destination_desc.clear_color = config.clear_color;
    std::unique_ptr<RenderTarget> destination = device.create_render_target(destination_desc);

    const SurfaceFrame frame = renderer.begin_frame(*destination);
    if (!frame) {
        return {};
    }

    auto pipeline =
        device.create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc(config.format));
    std::unique_ptr<tessera::rendering::rhi::Texture> white =
        device.create_texture(tessera::rendering::rhi::TextureDesc{{1, 1}});
    Batched2DConfig batched_config;
    batched_config.enable_aa = false;
    batched_config.golden_mode = true;
    Batched2DRenderer batched(device, *pipeline, *white, batched_config);
    batched.begin(*frame.commands, *frame.target, renderer.target_desc());
    paint_scene(batched);
    batched.end();

    if (renderer.end_frame() != SurfaceCompositeStatus::Composited) {
        return {};
    }
    // The composite was submitted asynchronously; this drain is the harness waiting on its own work
    // before reading, never the UI thread and never inside a frame.
    device.wait_idle();
    return read_target(device, *destination);
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

void test_capability_rules() {
    // The provenance rule itself, on plain data: no device needed, so it is checked on every lane
    // including the ones that skip the render.
    tessera::rendering::vulkan::VulkanDeviceInfo cpu;
    cpu.device_name = "llvmpipe (LLVM 21.1.2, 256 bits)";
    cpu.driver_name = "llvmpipe";
    cpu.device_type = 4; // VK_PHYSICAL_DEVICE_TYPE_CPU
    cpu.driver_id = 13;  // VK_DRIVER_ID_MESA_LLVMPIPE
    cpu.api_version = (1U << 22) | (4U << 12) | 318U;
    expect(cpu.software(), "a CPU device is a software device");
    expect(cpu.describe().find("software") != std::string::npos,
           "the description names the device class");
    expect(cpu.describe().find("llvmpipe") != std::string::npos,
           "the description names the driver");

    tessera::rendering::vulkan::VulkanDeviceInfo gpu;
    gpu.device_name = "NVIDIA GeForce RTX 4060";
    gpu.driver_name = "NVIDIA";
    gpu.device_type = 2; // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
    gpu.driver_id = 1;
    expect(!gpu.software(), "a discrete GPU is not a software device");
    expect(gpu.describe().find("hardware") != std::string::npos,
           "the description names a hardware device as such");
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "native_surface_golden: unknown argument " << argv[index] << "\n";
        return 2;
    }

    test_capability_rules();

    std::unique_ptr<Device> device;
    try {
        tessera::rendering::rhi::DeviceDesc device_desc;
        device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "native_surface_golden: skipped (no Vulkan device: " << error.what() << ")\n";
        return failures == 0 ? 0 : 1;
    }

    // Provenance before a single pixel: pinning or verifying a baseline on anything but a software
    // rasterizer is the one outcome that must never happen quietly.
    const std::optional<tessera::rendering::vulkan::VulkanDeviceInfo> info =
        tessera::rendering::vulkan::device_info(*device);
    if (!info) {
        std::cout << "native_surface_golden: skipped (device reports no Vulkan properties)\n";
        return failures == 0 ? 0 : 1;
    }
    std::cout << "native_surface_golden: device " << info->describe() << "\n";
    if (!info->software()) {
        std::cerr
            << "native_surface_golden: refused - not a software Vulkan device: " << info->describe()
            << "\n  A golden must be rendered on lavapipe (ADR-009): set VK_ICD_FILENAMES and "
               "VK_DRIVER_FILES to the lavapipe manifest and put that manifest's directory on "
               "PATH.\n";
        // Not a failure: this host has no software device and the baseline belongs to the lane that
        // has one. The refusal is loud so it can never be mistaken for a pass.
        std::cout << "native_surface_golden: skipped (no software Vulkan device on this host)\n";
        return failures == 0 ? 0 : 1;
    }

    const std::vector<std::uint8_t> first = render_surface_frame(*device);
    if (!expect(!first.empty(), "the composited surface frame rendered")) {
        std::cerr << "native_surface_golden: failed\n";
        return 1;
    }
    // Determinism proved, not assumed: two independent frames, byte-identical before either is
    // pinned. A baseline nobody can regenerate is a liability.
    const std::vector<std::uint8_t> second = render_surface_frame(*device);
    if (!expect(first == second, "two independent frames are byte-identical")) {
        std::cerr << "native_surface_golden: the surface path is not deterministic on this device; "
                     "refusing to pin pixels\n";
        return 1;
    }

    // A blank frame would be a baseline that always matches.
    bool painted = false;
    for (std::size_t i = 0; i < first.size(); i += 4) {
        if (first[i] != 17 || first[i + 1] != 24 || first[i + 2] != 39) {
            painted = true;
            break;
        }
    }
    if (!expect(painted, "the scene painted over the clear colour")) {
        return 1;
    }

    RgbaImage frame;
    frame.width = kExtent.width;
    frame.height = kExtent.height;
    frame.rgba = first;

    if (update_goldens) {
        const auto png = tessera::harness::encode_png(frame);
        if (!expect(png.has_value(), "the frame encodes as PNG")) {
            return 1;
        }
        if (!expect(write_file(TESSERA_SURFACE_GOLDEN_PATH, *png), "the baseline was written")) {
            return 1;
        }
        std::cout << "native_surface_golden: wrote " << TESSERA_SURFACE_GOLDEN_PATH << " from "
                  << info->describe() << "\n";
        return 0;
    }

    const std::vector<std::uint8_t> baseline_bytes = read_file(TESSERA_SURFACE_GOLDEN_PATH);
    if (!expect(!baseline_bytes.empty(), "the committed baseline exists")) {
        std::cerr << "native_surface_golden: regenerate it with --update-goldens on a software "
                     "device\n";
        return 1;
    }
    const auto baseline = tessera::harness::decode_png(baseline_bytes);
    if (!expect(baseline.has_value(), "the committed baseline decodes as PNG")) {
        return 1;
    }
    if (!expect(baseline->width == kExtent.width && baseline->height == kExtent.height,
                "the baseline has the surface's extent")) {
        return 1;
    }

    // Bit-exact: the surface composite is a format-preserving copy of a deterministic scene, so
    // there is no reason to accept a single differing byte.
    const ComparePolicy policy{0, 0.0};
    const tessera::harness::CompareResult result =
        tessera::harness::compare_images(CompareMode::Golden, frame, &*baseline, policy);
    if (!expect(result.passed(), "the composited surface is bit-identical to the baseline")) {
        std::cerr << "native_surface_golden: pixels=" << result.diff.pixels
                  << " mismatched=" << result.diff.mismatched
                  << " max_delta=" << result.diff.max_channel_delta
                  << " mean_delta=" << result.diff.mean_channel_delta()
                  << " outcome=" << tessera::harness::to_string(result.outcome)
                  << " device=" << info->describe() << "\n";
        return 1;
    }
    std::cout << "native_surface_golden: ok pixels=" << result.diff.pixels
              << " device=" << info->describe() << "\n";
    return 0;
}
