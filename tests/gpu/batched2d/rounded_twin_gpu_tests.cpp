// SPDX-License-Identifier: MIT
//
// The rounded-corner CPU twin against the renderer (issue #617): the `rounded` scene of the CPU
// corpus (a rounded linear gradient, a stadium radial gradient and a rounded solid fill) painted
// through the real paint pass on a software Vulkan device with `Batched2DSink` and with
// `CpuPaintSink`, in the renderer's two edge modes. Hard (golden mode) pins the pixel-centre
// rule; anti-aliased pins the half-pixel blend of the signed distance. Each pair must agree within
// `kTwinTolerance` per channel, so the arc, the radius clamp and the blend are the same on both
// sides. Hardware devices are refused with a skip, like the other lavapipe tests.

#include "cpu_wide_scene_support.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_device_info.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

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
using tessera::poc::batched2d::CpuPaintSink;
using tessera::poc::batched2d::cpu_wide_support::CpuCanvas;
using tessera::poc::batched2d::cpu_wide_support::kHeight;
using tessera::poc::batched2d::cpu_wide_support::kWidth;
using tessera::poc::batched2d::cpu_wide_support::StubStyles;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;

// Float-versus-byte rounding of the blend and the gradient ramp; measured on lavapipe.
constexpr int kTwinTolerance = 2;

int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "rounded_twin: " << message << "\n";
        ++failures;
    }
    return condition;
}

struct Scene {
    tessera::ui::Node root{1};
    StubStyles styles;
    Scene() {
        tessera::poc::batched2d::cpu_wide_support::rounded_cases().front().build(root, styles);
    }
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

std::vector<std::uint8_t> render_gpu(rhi::Device& device, rhi::Pipeline& pipeline,
                                     rhi::Texture& white, bool anti_aliased) {
    Scene scene;
    auto target = device.create_render_target(target_desc());
    auto commands = device.create_command_list();
    Batched2DConfig config;
    config.enable_aa = anti_aliased;
    config.golden_mode = !anti_aliased;
    {
        Batched2DRenderer renderer(device, pipeline, white, config);
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink);
        renderer.end();
        commands->submit();
    }
    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::vector<std::uint8_t> render_cpu(bool anti_aliased) {
    Scene scene;
    CpuCanvas canvas(kWidth, kHeight);
    CpuPaintSink sink(canvas);
    sink.set_aa_radius(anti_aliased ? 0.5f : 0.0f);
    (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink);
    return {canvas.pixels().begin(), canvas.pixels().end()};
}

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

int main() {
    std::unique_ptr<rhi::Device> device;
    try {
        rhi::DeviceDesc device_desc;
        device_desc.backend = rhi::Backend::Vulkan;
        device_desc.validation = true;
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "rounded_twin: skipped (no Vulkan device: " << error.what() << ")\n";
        return 0;
    }
    const auto info = tessera::rendering::vulkan::device_info(*device);
    if (!info || !info->software()) {
        std::cout << "rounded_twin: skipped (no software Vulkan device on this host)\n";
        return 0;
    }
    const auto pipeline = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(rhi::TextureFormat::R8G8B8A8Unorm));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    const auto white = device->create_texture(white_desc);

    for (const bool anti_aliased : {false, true}) {
        const char* mode = anti_aliased ? "anti-aliased" : "hard";
        const std::vector<std::uint8_t> gpu = render_gpu(*device, *pipeline, *white, anti_aliased);
        const std::vector<std::uint8_t> cpu = render_cpu(anti_aliased);
        std::size_t at = 0;
        const int worst = worst_difference(gpu, cpu, at);
        std::cout << "rounded_twin: " << mode << " worst channel delta " << worst << " at ("
                  << at % kWidth << "," << at / kWidth << ")\n";
        expect(worst <= kTwinTolerance, std::string(mode) + ": the CPU twin agrees with the GPU");
    }
    expect(tessera::rendering::vulkan::validation_message_count() == 0,
           "the run produced no validation messages");
    if (failures != 0) {
        return 1;
    }
    std::cout << "rounded_twin: ok\n";
    return 0;
}
