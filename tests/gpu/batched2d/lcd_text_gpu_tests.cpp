// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel test for LCD text compositing (#471, display-aware-rendering.md section 3.4.1).
//
// A glyph page texel carries three subpixel coverages in R, G and B (the maximum in alpha). Drawn
// through the LCD pipeline each channel blends with its own coverage, so over a grey backdrop:
//   out.c = ink.c * cov.c + backdrop.c * (1 - cov.c)
// while the same texel drawn through the grayscale pipeline reads only alpha and blends all three
// channels alike. The two must differ exactly the way that formula says, and the fallback
// (`push_texture_lcd` without a pipeline) must equal the grayscale result.
//
// Deterministic UNORM target, exact arithmetic within one rounding step; the test skips cleanly
// when no Vulkan device is available or the device cannot blend per channel (lavapipe can).

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
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

constexpr rhi::Extent2D kExtent{64, 64};
constexpr std::uint8_t kBackdrop = 40;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lcd_text_gpu: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = rhi::rgba8(kBackdrop, kBackdrop, kBackdrop);
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    return desc;
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool near(std::uint8_t actual, int expected, int tolerance = 2) {
    return std::abs(static_cast<int>(actual) - expected) <= tolerance;
}

// What the blend must produce for one channel: coverage in 0..255 over the grey backdrop.
int blended(int ink, int coverage) {
    return (ink * coverage + kBackdrop * (255 - coverage) + 127) / 255;
}

// A uniform page: every texel is one subpixel triple (R, G, B) with the strongest in alpha, the
// layout `GlyphAtlas::upload` writes for an LCD glyph.  A grayscale page is the classic layout:
// white RGB and coverage in alpha only (`make_page_pixels`).
std::unique_ptr<rhi::Texture> page_with(rhi::Device& device, std::uint8_t r, std::uint8_t g,
                                        std::uint8_t b) {
    std::vector<std::uint8_t> texels;
    const std::uint8_t strongest = std::max(r, std::max(g, b));
    for (int i = 0; i < 4 * 4; ++i) {
        texels.insert(texels.end(), {r, g, b, strongest});
    }
    rhi::TextureDesc desc;
    desc.extent = {4, 4};
    desc.pixels = texels.data();
    desc.row_pitch = 4 * 4;
    return device.create_texture(desc);
}

std::unique_ptr<rhi::Texture> gray_page(rhi::Device& device) {
    return page_with(device, 255, 255, 255);
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
        std::cout << "lcd_text_gpu: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }
    if (!device->supports_dual_source_blend()) {
        std::cout << "lcd_text_gpu: skipped (device reports no dualSrcBlend)\n";
        return 0;
    }

    const rhi::RenderTargetDesc desc = target_desc();
    auto target = device->create_render_target(desc);
    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    auto lcd = device->create_pipeline(tessera::rendering::vulkan::batched2d_lcd_pipeline_desc());

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    // AA off + golden mode: hard edges and exact bytes.
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;

    // Coverage (255, 128, 0): a stem that is fully on in the red subpixel, half on in green, off in
    // blue. White ink, so each channel's result is the coverage blended toward 255 over the
    // backdrop.
    auto page = page_with(*device, 255, 128, 0);
    const rhi::Rect uv{0, 0, 1, 1};
    const rhi::Color ink = rhi::rgba8(255, 255, 255);

    auto plain = gray_page(*device);
    bool ok = true;

    // With the LCD pipeline: a per-channel run, the same page as one coverage (a grayscale run or
    // a glyph FreeType could not render as LCD), a classic grayscale glyph through the ordinary
    // pipeline, and a rect after the LCD batches (the frame pipeline must be bound again).
    {
        Batched2DRenderer renderer(*device, *pipeline, *white, config);
        renderer.set_lcd_pipeline(lcd.get());
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        renderer.push_texture_lcd({4, 4, 4, 4}, *page, uv, ink, 1.0f, true);
        renderer.push_texture_lcd({12, 4, 4, 4}, *page, uv, ink, 1.0f, false);
        renderer.push_texture({20, 4, 4, 4}, *plain, uv, ink);
        renderer.push_rect({40, 40, 8, 8}, rhi::rgba8(255, 0, 0));
        renderer.end();
        commands->submit();
        device->wait_idle();
        const std::vector<std::uint8_t> frame = read_target(*device, *target);

        const auto fringed = pixel_at(frame, 5, 5);
        ok = expect(near(fringed[0], blended(255, 255)) && near(fringed[1], blended(255, 128)) &&
                        near(fringed[2], blended(255, 0)),
                    "the LCD pipeline blends each channel with its own coverage") &&
             ok;
        ok = expect(fringed[0] > fringed[1] && fringed[1] > fringed[2],
                    "the three channels come out different (the fringe)") &&
             ok;
        const auto single = pixel_at(frame, 13, 5);
        ok = expect(near(single[0], 255) && near(single[1], 255) && near(single[2], 255),
                    "a non-per-channel LCD quad reads alpha as one coverage for every channel") &&
             ok;
        const auto gray = pixel_at(frame, 21, 5);
        ok = expect(near(gray[0], 255) && near(gray[1], 255) && near(gray[2], 255),
                    "the ordinary pipeline still draws a grayscale page") &&
             ok;
        const auto rect = pixel_at(frame, 44, 44);
        ok = expect(near(rect[0], 255, 0) && near(rect[1], 0, 0) && near(rect[2], 0, 0),
                    "a rect after the LCD batches draws with the frame pipeline") &&
             ok;
        ok = expect(near(pixel_at(frame, 30, 5)[0], kBackdrop, 0),
                    "pixels outside every quad keep the backdrop") &&
             ok;
    }

    // Without an LCD pipeline the push is `push_texture`: a grayscale page draws exactly as before.
    {
        Batched2DRenderer renderer(*device, *pipeline, *white, config);
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        renderer.push_texture_lcd({4, 4, 4, 4}, *plain, uv, ink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        const auto pixel = pixel_at(read_target(*device, *target), 5, 5);
        ok = expect(near(pixel[0], 255) && near(pixel[1], 255) && near(pixel[2], 255),
                    "without a pipeline the LCD push is the ordinary push") &&
             ok;
    }

    if (!ok) {
        std::cerr << "lcd_text_gpu: failed\n";
        return 1;
    }
    std::cout << "lcd_text_gpu: ok\n";
    return 0;
}
