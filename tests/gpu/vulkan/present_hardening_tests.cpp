// SPDX-License-Identifier: MIT
//
// Real-GPU present-path hardening smoke: N frames of a scene combining a rounded clip, rounded
// rect, rotated quad, and wrapped text through the batched2d pipeline; the non-MSAA path must be
// byte-identical across frames/runs. When the device supports 4x, one frame renders into a 4x
// target and reads back the resolved image. Then a simulated device loss is recovered (D7
// controller), resources are recreated, and the same scene must render byte-identically again.
// Validation layers are enabled; skips cleanly when Vulkan, test hooks, the fixture font, or 4x are
// unavailable.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#if defined(TESSERA_VULKAN_TEST_HOOKS)
#include "vulkan_internal.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::Backend;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::DeviceDesc;
using tessera::rendering::rhi::DeviceStatus;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr Extent2D kExtent{192, 128};
constexpr std::uint32_t kFrames = 3;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "present_hardening: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc(std::uint32_t sample_count) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    desc.sample_count = sample_count;
    return desc;
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

void draw_hardened_scene(Batched2DRenderer& renderer, TextRenderer& text,
                         const HarfBuzzShaper& shaper) {
    using tessera::rendering::rhi::rgba8;
    const std::string paragraph = "The quick brown fox jumps over the lazy dog near the river bank";

    // Rounded clip + rounded rect: the clip applies to the rotated quad and the text below.
    const tessera::rendering::gpu::clip::RoundedRect clip{
        {8.0f, 8.0f, 176.0f, 112.0f}, 12.0f, 12.0f, 12.0f, 12.0f};
    renderer.push_clip(clip);
    renderer.push_rounded_rect({8.0f, 8.0f, 176.0f, 112.0f}, rgba8(24, 32, 48), 12.0f);

    // Wrapped text inside the rounded clip: line boxes from the shared layout, drawn per line.
    TextStyle style;
    style.font_size = 13.0f;
    tessera::ui::text::TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = 140.0f;
    const tessera::ui::text::TextLayout layout = tessera::ui::text::layout_text(
        paragraph, style, tessera::ui::text::StubFontMetricsProvider{}.metrics(style), constraints);
    float line_top = 18.0f;
    for (const tessera::ui::text::TextLine& line : layout.lines) {
        if (line.end > line.begin) {
            const std::string_view slice =
                std::string_view(paragraph).substr(line.begin, line.end - line.begin);
            const tessera::ui::text::ShapedRun run = shaper.shape(slice, style);
            text.draw(renderer, run, 16.0f, line_top + line.baseline, 13, rgba8(226, 232, 240));
        }
        line_top += line.height;
    }

    // Rotated quad: exercises MSAA edge coverage on non-SDF geometry.
    renderer.push_transform(Mat3::translation(160.0f, 88.0f) * Mat3::rotation(0.45f));
    renderer.push_rect({-26.0f, -26.0f, 52.0f, 52.0f}, rgba8(56, 189, 248));
    renderer.pop_transform();
    renderer.pop_clip();
}

// Renders one frame of the hardened scene into a fresh target and returns the readback.
std::vector<std::uint8_t> render_frame(Device& device, std::uint32_t sample_count,
                                       const FontFace& face) {
    RenderTargetDesc desc = target_desc(sample_count);
    auto target = device.create_render_target(desc);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc(
        TextureFormat::R8G8B8A8Unorm, sample_count);
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    GlyphRasterizer rasterizer(face);
    HarfBuzzShaper shaper(face);
    TextRenderer text(device, rasterizer, {256, 256, 2, 1});
    Batched2DConfig config{4096, false, true, sample_count};
    Batched2DRenderer renderer(device, *pipeline, *white, config);

    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, desc);
    text.begin_frame();
    draw_hardened_scene(renderer, text, shaper);
    renderer.end();
    commands->submit();
    device.wait_idle();
    return read_target(device, *target);
}

bool same_pixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    return a == b;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(TESSERA_VULKAN_TEST_HOOKS)
    std::cout << "present_hardening: skipped (TESSERA_VULKAN_TEST_HOOKS disabled)\n";
    return 0;
#else
    if (argc < 2) {
        std::cout << "present_hardening: skipped (missing font fixture path)\n";
        return 0;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cout << "present_hardening: skipped (cannot load fixture: " << loaded.error().message
                  << ")\n";
        return 0;
    }
    const FontFace face = *loaded;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "present_hardening: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    // Non-MSAA determinism: N frames of the combined scene must be byte-identical, and a second
    // run of the same scene must match the first run exactly.
    std::vector<std::vector<std::uint8_t>> frames;
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        frames.push_back(render_frame(*device, 1, face));
    }
    bool ok = true;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        ok = expect(same_pixels(frames[0], frames[index]), "non-MSAA frames are byte-identical") &&
             ok;
    }
    const std::vector<std::uint8_t> repeat = render_frame(*device, 1, face);
    ok = expect(same_pixels(frames[0], repeat), "non-MSAA repeat run is byte-identical") && ok;

    std::string msaa_result = "skipped";
    if (device->supports_sample_count(TextureFormat::R8G8B8A8Unorm, 4)) {
        const std::vector<std::uint8_t> msaa = render_frame(*device, 4, face);
        std::size_t non_clear = 0;
        for (std::size_t i = 0; i + 3 < msaa.size(); i += 4) {
            if (msaa[i] != 0 || msaa[i + 1] != 0 || msaa[i + 2] != 0) {
                ++non_clear;
            }
        }
        ok = expect(non_clear > 0, "4x frame renders into its resolved image") && ok;
        msaa_result = "4x";
    } else {
        std::cout << "present_hardening: skip 4x (R8G8B8A8Unorm sample_count=4 unsupported)\n";
    }

    // Device-loss cycle: all scene resources are already released (render_frame is scoped). Inject
    // a simulated loss, recover through the D7 controller, then render the same scene again and
    // require the pixels to match the pre-loss reference.
    auto* vulkan_device = dynamic_cast<tessera::rendering::vulkan::VulkanDevice*>(device.get());
    if (vulkan_device == nullptr) {
        std::cout << "present_hardening: skipped (device is not the Vulkan backend)\n";
        return 0;
    }
    vulkan_device->simulate_device_loss_for_testing();
    ok = expect(device->status() == DeviceStatus::Lost, "simulated loss marks the device lost") &&
         ok;

    tessera::runtime::DeviceRecoveryController controller(*device);
    const tessera::runtime::DeviceRecoveryUpdate update = controller.tick();
    ok = expect(update.loss_observed && update.recovered && update.full_frame_required,
                "controller recovers after the caller released its resources") &&
         ok;
    ok = expect(device->status() == DeviceStatus::Ready, "recovered device is Ready") && ok;

    const std::vector<std::uint8_t> after_recovery = render_frame(*device, 1, face);
    ok = expect(same_pixels(frames[0], after_recovery),
                "post-recovery render matches the pre-loss reference") &&
         ok;

    if (!ok) {
        std::cerr << "present_hardening: failed\n";
        return 1;
    }
    std::cout << "present_hardening: ok (frames=" << kFrames << " msaa=" << msaa_result
              << " recovered=1 generation=" << update.generation << ")\n";
    return 0;
#endif
}
