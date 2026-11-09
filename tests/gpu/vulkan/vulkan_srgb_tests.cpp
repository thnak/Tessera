// SPDX-License-Identifier: MIT
//
// D2 sRGB color policy on a real Vulkan device: an sRGB render target encodes fragment output from
// linear light to sRGB on write, while the same draw into an UNORM target keeps the historical
// (byte-identical) behavior. The test skips cleanly when no Vulkan device or no sRGB target format
// is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;

constexpr Extent2D kExtent{4, 4};
constexpr Color kProbe{0.25f, 0.5f, 0.75f, 1.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_srgb: " << message << "\n";
    }
    return condition;
}

// 8-bit UNORM encoding of a shader-output channel (round to nearest).
int unorm8(float linear) {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 255.0f));
}

// 8-bit sRGB encoding of a linear shader-output channel (the GPU's write conversion).
int srgb8(float linear) {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const float encoded =
        clamped <= 0.0031308f ? 12.92f * clamped : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(encoded * 255.0f));
}

RenderTargetDesc target_desc(TextureFormat format) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = format;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    return desc;
}

// Draws one opaque full-target quad and returns the raw 8-bit readback (RGBA or BGRA byte order,
// matching the format).
std::vector<std::uint8_t> render_probe(Device& device, TextureFormat format) {
    const RenderTargetDesc desc = target_desc(format);
    auto target = device.create_render_target(desc);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = format;
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, desc);
    renderer.push_rect(
        {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)},
        kProbe);
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto copy_commands = device.create_command_list();
    copy_commands->copy_target_to_buffer(*target, *readback);
    copy_commands->submit();
    device.wait_idle();

    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

bool channels_match(const std::vector<std::uint8_t>& pixels, const std::array<int, 4>& expected,
                    int tolerance) {
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (std::abs(static_cast<int>(pixels[i + channel]) - expected[channel]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

bool create_srgb_textures(Device& device) {
    for (const TextureFormat format : {TextureFormat::R8G8B8A8Srgb, TextureFormat::B8G8R8A8Srgb}) {
        const std::array<std::uint8_t, 4> texel = {10, 20, 30, 40};
        TextureDesc desc;
        desc.extent = {1, 1};
        desc.format = format;
        desc.pixels = texel.data();
        desc.row_pitch = 4;
        try {
            const auto texture = device.create_texture(desc);
            if (!expect(texture->format() == format, "sRGB texture keeps its format")) {
                return false;
            }
            const std::array<std::uint8_t, 4> updated = {1, 2, 3, 4};
            texture->write(updated.data(), updated.size());
        } catch (const std::exception&) {
            // A device may lack sampled sRGB support even when it can render to sRGB.
            std::cout << "vulkan_srgb: sampled sRGB textures unavailable, skipped\n";
        }
    }
    return true;
}

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    // The deterministic golden/offscreen path must keep its UNORM defaults byte-for-byte.
    bool ok = expect(RenderTargetDesc{}.format == TextureFormat::R8G8B8A8Unorm,
                     "render targets default to UNORM (golden path)") &&
              expect(tessera::rendering::vulkan::batched2d_pipeline_desc().color_format ==
                         TextureFormat::R8G8B8A8Unorm,
                     "the batched2d pipeline defaults to UNORM");

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = false;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_srgb: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return ok ? 0 : 1;
    }

    ok = create_srgb_textures(*device) && ok;

    // The UNORM target is the historical path: shader output is stored as-is (goldens rely on it).
    const std::vector<std::uint8_t> unorm = render_probe(*device, TextureFormat::R8G8B8A8Unorm);
    ok = expect(channels_match(unorm, {unorm8(0.25f), unorm8(0.5f), unorm8(0.75f), 255}, 1),
                "UNORM target keeps the raw shader output") &&
         ok;

    // sRGB targets encode the same linear output; the byte order follows the format layout.
    bool any_srgb = false;
    try {
        const std::vector<std::uint8_t> rgba = render_probe(*device, TextureFormat::R8G8B8A8Srgb);
        any_srgb = true;
        ok = expect(channels_match(rgba, {srgb8(0.25f), srgb8(0.5f), srgb8(0.75f), 255}, 2),
                    "R8G8B8A8Srgb encodes shader output to sRGB") &&
             ok;
    } catch (const std::exception& error) {
        std::cout << "vulkan_srgb: R8G8B8A8Srgb target unavailable (" << error.what() << ")\n";
    }
    try {
        const std::vector<std::uint8_t> bgra = render_probe(*device, TextureFormat::B8G8R8A8Srgb);
        any_srgb = true;
        ok = expect(channels_match(bgra, {srgb8(0.75f), srgb8(0.5f), srgb8(0.25f), 255}, 2),
                    "B8G8R8A8Srgb encodes and keeps BGRA byte order") &&
             ok;
    } catch (const std::exception& error) {
        std::cout << "vulkan_srgb: B8G8R8A8Srgb target unavailable (" << error.what() << ")\n";
    }

    if (!any_srgb) {
        std::cout << "vulkan_srgb: skipped (no sRGB render target format on this device)\n";
        return ok ? 0 : 1;
    }
    if (!ok) {
        std::cerr << "vulkan_srgb: failed\n";
        return 1;
    }
    std::cout << "vulkan_srgb: ok\n";
    return 0;
}
