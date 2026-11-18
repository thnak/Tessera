// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel test for texture mip chains (#491, RHI 1.6, display-aware-rendering.md section
// 4). A 96x96 one-pixel checkerboard is drawn into a 32x32 rect, a 3x minification. At that ratio a
// level-0 sample lands exactly on a texel centre, so a single-level texture reads pure black or
// pure white and aliases; the same texture with a full chain reads the grey average of the pattern.
//
// Deterministic UNORM target on a software or hardware device alike: the assertion is a band around
// mid-grey, not a golden. Skips cleanly when no Vulkan device is available or the device cannot
// blit the format.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;

constexpr rhi::Extent2D kRaster{96, 96};
constexpr rhi::Extent2D kTarget{32, 32};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "texture_mips_gpu: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> checkerboard() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kRaster.width) * kRaster.height * 4);
    for (std::uint32_t y = 0; y < kRaster.height; ++y) {
        for (std::uint32_t x = 0; x < kRaster.width; ++x) {
            const std::uint8_t value = ((x + y) % 2U) == 0U ? 255 : 0;
            const std::size_t index = (static_cast<std::size_t>(y) * kRaster.width + x) * 4;
            pixels[index] = value;
            pixels[index + 1] = value;
            pixels[index + 2] = value;
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

std::unique_ptr<rhi::Texture> make_texture(rhi::Device& device, const std::vector<std::uint8_t>& px,
                                           std::uint32_t levels) {
    rhi::TextureDesc desc;
    desc.extent = kRaster;
    desc.pixels = px.data();
    desc.row_pitch = static_cast<std::size_t>(kRaster.width) * 4;
    desc.filter = rhi::Filter::Linear;
    desc.levels = levels;
    return device.create_texture(desc);
}

// Draws the texture into the whole target and returns the red channel of every pixel.
std::vector<std::uint8_t> draw_minified(rhi::Device& device, rhi::Pipeline& pipeline,
                                        rhi::Texture& white, rhi::Texture& texture) {
    rhi::RenderTargetDesc desc;
    desc.extent = kTarget;
    desc.clear_color = rhi::rgba8(0, 0, 255);
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    auto target = device.create_render_target(desc);

    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Batched2DRenderer renderer(device, pipeline, white, config);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, desc);
    renderer.push_texture({0, 0, 32, 32}, texture, {0, 0, 1, 1}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(kTarget.width) * kTarget.height * 4;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    std::vector<std::uint8_t> frame(bytes);
    readback->read(frame.data(), frame.size());
    std::vector<std::uint8_t> red(static_cast<std::size_t>(kTarget.width) * kTarget.height);
    for (std::size_t i = 0; i < red.size(); ++i) {
        red[i] = frame[i * 4];
    }
    return red;
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
        std::cout << "texture_mips_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!device->supports_texture_mipmaps(rhi::TextureFormat::R8G8B8A8Unorm)) {
        std::cout << "texture_mips_gpu: skipped (device cannot blit the format)\n";
        return 0;
    }

    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    const std::vector<std::uint8_t> pixels = checkerboard();
    const std::uint32_t full = rhi::full_mip_levels(kRaster);
    bool ok = expect(full == 7, "96x96 has a seven-level chain");

    auto flat = make_texture(*device, pixels, 1);
    auto chained = make_texture(*device, pixels, full);
    ok = expect(flat != nullptr && flat->levels() == 1, "a default texture has one level") && ok;
    ok = expect(chained != nullptr && chained->levels() == full,
                "the chain texture reports its level count") &&
         ok;
    if (flat == nullptr || chained == nullptr) {
        return 1;
    }

    // Control: one level aliases, so most pixels are pure black or pure white.
    const std::vector<std::uint8_t> aliased = draw_minified(*device, *pipeline, *white, *flat);
    std::size_t extreme = 0;
    for (const std::uint8_t value : aliased) {
        extreme += (value < 32 || value > 223) ? 1U : 0U;
    }
    ok = expect(extreme * 10 >= aliased.size() * 9,
                "control: a single level aliases at a 3x downscale (the test discriminates)") &&
         ok;

    // With the chain every pixel is the grey average of the checkerboard.
    const std::vector<std::uint8_t> filtered = draw_minified(*device, *pipeline, *white, *chained);
    std::size_t outside_band = 0;
    for (const std::uint8_t value : filtered) {
        outside_band += (value < 118 || value > 138) ? 1U : 0U;
    }
    ok = expect(outside_band == 0, "with a mip chain a 3x downscale stays in the grey band") && ok;

    // The chain is immutable: a write would leave the lower levels stale.
    bool write_rejected = false;
    try {
        chained->write(pixels.data(), pixels.size());
    } catch (const std::invalid_argument&) {
        write_rejected = true;
    }
    ok = expect(write_rejected, "writing a mipmapped texture is rejected") && ok;

    // Malformed requests are rejected before the device is touched.
    bool too_many = false;
    try {
        (void)make_texture(*device, pixels, full + 1);
    } catch (const std::invalid_argument&) {
        too_many = true;
    }
    ok = expect(too_many, "more levels than a full chain is rejected") && ok;

    if (!ok) {
        std::cerr << "texture_mips_gpu: failed\n";
        return 1;
    }
    std::cout << "texture_mips_gpu: ok\n";
    return 0;
}
