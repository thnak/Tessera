// SPDX-License-Identifier: MIT
//
// `Texture::write_region` (RHI 1.9, issue #681) on Vulkan, read back through the pipeline: the
// rectangle lands at the right place with the right bytes (tightly packed and from a wider pitched
// source), every texel outside it is untouched, and a texture written region by region equals one
// created whole. The texture is drawn 1:1 with a Nearest filter, which the exactness measurement
// (#687) showed is byte-exact at identity scale. Skips cleanly when no Vulkan device is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gpu = tessera::rendering::gpu;

constexpr std::uint32_t kWidth = 32;
constexpr std::uint32_t kHeight = 24;

[[nodiscard]] bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "texture_write_region: " << message << "\n";
    }
    return condition;
}

// Opaque noise: any wrong byte, offset or row shows.
[[nodiscard]] std::vector<std::uint8_t> noise(std::size_t pixels, std::uint32_t seed) {
    std::vector<std::uint8_t> bytes(pixels * 4U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        bytes[i] = (i % 4U == 3U) ? std::uint8_t{255} : static_cast<std::uint8_t>(seed >> 24);
    }
    return bytes;
}

// Draws `texture` 1:1 into a kWidth x kHeight target and returns the readback.
[[nodiscard]] std::vector<std::uint8_t> read_back(rhi::Device& device, rhi::Texture& texture) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {kWidth, kHeight};
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    auto target = device.create_render_target(target_desc);

    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = target_desc.format;
    auto pipeline = device.create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);

    gpu::Batched2DRenderer renderer(device, *pipeline, *white);
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, target_desc);
    renderer.push_texture({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)},
                          texture, {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(kWidth) * kHeight * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

[[nodiscard]] std::unique_ptr<rhi::Texture> make_texture(rhi::Device& device,
                                                         const std::vector<std::uint8_t>& pixels) {
    rhi::TextureDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.pixels = pixels.data();
    desc.row_pitch = static_cast<std::size_t>(kWidth) * 4U;
    desc.filter = rhi::Filter::Nearest;
    auto texture = device.create_texture(desc);
    device.wait_idle();
    return texture;
}

// Copies the rectangle of `from` into `into` (both kWidth wide), the CPU model of a region write.
void paste(std::vector<std::uint8_t>& into, const std::vector<std::uint8_t>& from,
           const rhi::TextureRegion& region) {
    for (std::uint32_t row = 0; row < region.height; ++row) {
        for (std::uint32_t column = 0; column < region.width; ++column) {
            const std::size_t index =
                (static_cast<std::size_t>(region.y + row) * kWidth + region.x + column) * 4U;
            for (std::size_t c = 0; c < 4; ++c) {
                into[index + c] = from[index + c];
            }
        }
    }
}

bool run(rhi::Device& device) {
    const auto base = noise(static_cast<std::size_t>(kWidth) * kHeight, 1U);
    const auto patch = noise(static_cast<std::size_t>(kWidth) * kHeight, 2U);
    bool ok = true;

    // (a) Pitched source: the rectangle's rows sit inside a kWidth-wide image, the way an atlas
    // page does. The first row of the region is passed, with the page's row pitch.
    {
        const rhi::TextureRegion region{5, 3, 11, 7};
        auto texture = make_texture(device, base);
        const std::size_t first = (static_cast<std::size_t>(region.y) * kWidth + region.x) * 4U;
        texture->write_region(region, patch.data() + first, static_cast<std::size_t>(kWidth) * 4U);
        device.wait_idle();
        auto expected = base;
        paste(expected, patch, region);
        ok = expect(read_back(device, *texture) == expected,
                    "a pitched region write changes exactly the rectangle") &&
             ok;
    }

    // (b) Tightly packed source (pitch 0): the staging layout the backend builds for (a) too.
    {
        const rhi::TextureRegion region{20, 10, 9, 13};
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(region.width) * region.height *
                                         4U);
        for (std::uint32_t row = 0; row < region.height; ++row) {
            for (std::uint32_t column = 0; column < region.width; ++column) {
                const std::size_t from =
                    (static_cast<std::size_t>(region.y + row) * kWidth + region.x + column) * 4U;
                const std::size_t to = (static_cast<std::size_t>(row) * region.width + column) * 4U;
                for (std::size_t c = 0; c < 4; ++c) {
                    packed[to + c] = patch[from + c];
                }
            }
        }
        auto texture = make_texture(device, base);
        texture->write_region(region, packed.data(), 0);
        device.wait_idle();
        auto expected = base;
        paste(expected, patch, region);
        ok = expect(read_back(device, *texture) == expected,
                    "a tightly packed region write changes exactly the rectangle") &&
             ok;
    }

    // (c) The far corner and a one-texel region: no off-by-one at either end of the extent.
    {
        auto texture = make_texture(device, base);
        auto expected = base;
        for (const rhi::TextureRegion& region :
             {rhi::TextureRegion{kWidth - 1, kHeight - 1, 1, 1}, rhi::TextureRegion{0, 0, 1, 1},
              rhi::TextureRegion{kWidth - 4, 0, 4, 3}}) {
            const std::size_t first = (static_cast<std::size_t>(region.y) * kWidth + region.x) * 4U;
            texture->write_region(region, patch.data() + first,
                                  static_cast<std::size_t>(kWidth) * 4U);
            paste(expected, patch, region);
        }
        device.wait_idle();
        ok = expect(read_back(device, *texture) == expected,
                    "corner and one-texel region writes land exactly") &&
             ok;
    }

    // (d) A texture written region by region equals one created whole from the same bytes.
    {
        std::vector<std::uint8_t> blank(base.size(), 0);
        auto texture = make_texture(device, blank);
        for (std::uint32_t y = 0; y < kHeight; y += 8) {
            const rhi::TextureRegion band{0, y, kWidth, 8};
            texture->write_region(band, base.data() + static_cast<std::size_t>(y) * kWidth * 4U,
                                  static_cast<std::size_t>(kWidth) * 4U);
        }
        device.wait_idle();
        auto whole = make_texture(device, base);
        ok = expect(read_back(device, *texture) == read_back(device, *whole),
                    "a texture filled by bands equals one created whole") &&
             ok;
    }

    // (e) An empty region and a refused one leave the texture unchanged.
    {
        auto texture = make_texture(device, base);
        texture->write_region({4, 4, 0, 3}, nullptr, 0);
        bool refused = false;
        try {
            texture->write_region({kWidth - 1, 0, 2, 2}, patch.data(), 0);
        } catch (const std::out_of_range&) {
            refused = true;
        }
        device.wait_idle();
        ok = expect(refused, "a region past the edge is refused") && ok;
        ok = expect(read_back(device, *texture) == base,
                    "an empty or refused region write leaves the texture unchanged") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "texture_write_region: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!run(*device)) {
        std::cerr << "texture_write_region: FAILED\n";
        return 1;
    }
    std::cout << "texture_write_region: ok\n";
    return 0;
}
