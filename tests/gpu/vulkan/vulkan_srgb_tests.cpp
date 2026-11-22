// SPDX-License-Identifier: MIT
//
// The presentation color policy (issue #791) on a real Vulkan device, plus the underlying transfer
// functions it relies on.
//
// The policy: the paint pipeline's colors are already sRGB-encoded (an authored `#2563eb` reaches
// the shader as `37/255, 99/255, 235/255`), so an `*_SRGB` attachment with its linear->sRGB write
// conversion encodes them a second time and every color washes out. Presentation therefore uses a
// UNORM surface, where the shader's bytes reach the screen unchanged. The same draw into an
// `*_SRGB` target proves the double encoding the policy avoids - that path stays available for a
// caller that really holds linear light.
//
// Skips cleanly when no Vulkan device or no sRGB target format is available.

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

namespace rhi = tessera::rendering::rhi;

constexpr Extent2D kExtent{4, 4};
// The authored colours from issue #791, as the stylesheet writes them. An opaque full-target quad
// of any of these must read back byte-identical from a UNORM target: that is the whole point of the
// presentation policy.
constexpr std::array<std::array<std::uint8_t, 3>, 3> kAuthoredColors = {
    {{0x0b, 0x12, 0x20}, {0x25, 0x63, 0xeb}, {0x1e, 0x29, 0x3b}}};
// One unambiguous mid-range colour for the transfer-function comparisons below.
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

// Draws one opaque full-target quad of `color` and returns the raw 8-bit readback (RGBA or BGRA
// byte order, matching the format).
std::vector<std::uint8_t> render_probe(Device& device, TextureFormat format, Color color) {
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
        {0.0f, 0.0f, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)}, color);
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

// `label` (optional) is printed with the mismatching pixel, so a regression in the presentation
// color policy names the authored color that moved instead of just failing a count.
bool channels_match(const std::vector<std::uint8_t>& pixels, const std::array<int, 4>& expected,
                    int tolerance, const char* label) {
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const int actual = static_cast<int>(pixels[i + channel]);
            if (std::abs(actual - expected[channel]) > tolerance) {
                if (label != nullptr) {
                    std::cerr << "vulkan_srgb: " << label << " channel " << channel << " expected "
                              << expected[channel] << " got " << actual << "\n";
                }
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

    // The presentation policy itself (issue #791): a UNORM target - the format every presentation
    // surface uses - must read back the authored bytes unchanged, for the exact colours the issue
    // measured. This is the assertion that fails if presentation ever goes back to `*_SRGB`.
    for (const auto& authored : kAuthoredColors) {
        const Color color = rhi::rgba8(authored[0], authored[1], authored[2]);
        const std::vector<std::uint8_t> got =
            render_probe(*device, TextureFormat::R8G8B8A8Unorm, color);
        ok = expect(channels_match(got, {authored[0], authored[1], authored[2], 255}, 1,
                                   "authored color on a UNORM target"),
                    "a UNORM target shows authored colors unchanged") &&
             ok;
    }

    // The UNORM target stores shader output as-is (goldens rely on it).
    const std::vector<std::uint8_t> unorm =
        render_probe(*device, TextureFormat::R8G8B8A8Unorm, kProbe);
    ok = expect(channels_match(unorm, {unorm8(0.25f), unorm8(0.5f), unorm8(0.75f), 255}, 1,
                               "UNORM raw output"),
                "UNORM target keeps the raw shader output") &&
         ok;

    // sRGB targets encode the same output, and the byte order follows the format layout.
    bool any_srgb = false;
    try {
        const std::vector<std::uint8_t> rgba =
            render_probe(*device, TextureFormat::R8G8B8A8Srgb, kProbe);
        any_srgb = true;
        ok = expect(channels_match(rgba, {srgb8(0.25f), srgb8(0.5f), srgb8(0.75f), 255}, 2,
                                   "R8G8B8A8Srgb encoded output"),
                    "R8G8B8A8Srgb encodes shader output to sRGB") &&
             ok;
    } catch (const std::exception& error) {
        std::cout << "vulkan_srgb: R8G8B8A8Srgb target unavailable (" << error.what() << ")\n";
    }
    try {
        const std::vector<std::uint8_t> bgra =
            render_probe(*device, TextureFormat::B8G8R8A8Srgb, kProbe);
        any_srgb = true;
        ok = expect(channels_match(bgra, {srgb8(0.75f), srgb8(0.5f), srgb8(0.25f), 255}, 2,
                                   "B8G8R8A8Srgb encoded output"),
                    "B8G8R8A8Srgb encodes and keeps BGRA byte order") &&
             ok;

        // Where a presentation surface would land if it ever chose sRGB again: the authored bytes
        // come back re-encoded, which is the wash-out issue #791 measured. Pinning it here means
        // the cost of that choice stays visible and the policy has a numeric justification.
        const Color authored = rhi::rgba8(0x25, 0x63, 0xeb); // the shell's button, #2563eb
        const std::vector<std::uint8_t> double_encoded =
            render_probe(*device, TextureFormat::B8G8R8A8Srgb, authored);
        ok = expect(channels_match(
                        double_encoded,
                        {srgb8(0xeb / 255.0f), srgb8(0x63 / 255.0f), srgb8(0x25 / 255.0f), 255}, 2,
                        "double-encoded #2563eb"),
                    "an sRGB target encodes authored colors a second time") &&
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
