// SPDX-License-Identifier: MIT
//
// `Batched2DRenderer::push_texture_exact` (issue #682): integer-exact nearest sampling. The result
// must equal the CPU rule `min(floor((2p + 1) * src / (2 * dst)), src - 1)` byte for byte, at the
// ratios where the hardware nearest sampler is not exact (a 2:1 downscale puts every sample on a
// texel boundary, a fractional ratio puts a few there), with the quad at an offset inside a larger
// target, and at the 4K -> 1080p ratio the image path actually produces. Skips cleanly when no
// Vulkan device is available.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <algorithm>
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

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "texture_exact: " << message << "\n";
    }
    return condition;
}

// Random opaque noise: any wrong neighbor changes a byte.
[[nodiscard]] std::vector<std::uint8_t> noise(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    std::uint32_t seed = 987654321U;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        pixels[i] = (i % 4U == 3U) ? std::uint8_t{255} : static_cast<std::uint8_t>(seed >> 24);
    }
    return pixels;
}

// The CPU rule, the same arithmetic as `resample_nearest` and the shader.
[[nodiscard]] std::vector<std::uint8_t> cpu_nearest(const std::vector<std::uint8_t>& source,
                                                    std::uint32_t source_width,
                                                    std::uint32_t source_height,
                                                    std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint64_t ny = (2ULL * y + 1ULL) * source_height;
        const std::uint32_t sy = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(ny / (2ULL * height)), source_height - 1U);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint64_t nx = (2ULL * x + 1ULL) * source_width;
            const std::uint32_t sx = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(nx / (2ULL * width)), source_width - 1U);
            const std::size_t s = (static_cast<std::size_t>(sy) * source_width + sx) * 4U;
            const std::size_t d = (static_cast<std::size_t>(y) * width + x) * 4U;
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(s), 4, out.begin() + d);
        }
    }
    return out;
}

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8, the whole target
};

// Draws `texture` with push_texture_exact into `rect` of a `target_width` x `target_height`
// target cleared to opaque black, and returns the readback.
[[nodiscard]] Frame draw_exact(rhi::Device& device, rhi::Texture& texture,
                               std::uint32_t target_width, std::uint32_t target_height,
                               const rhi::Rect& rect) {
    rhi::RenderTargetDesc target_desc;
    target_desc.extent = {target_width, target_height};
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
    renderer.push_texture_exact(rect, texture, rhi::rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
    device.wait_idle();

    const std::size_t bytes = static_cast<std::size_t>(target_width) * target_height * 4U;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto copy = device.create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();
    device.wait_idle();

    Frame frame;
    frame.width = target_width;
    frame.height = target_height;
    frame.pixels.resize(bytes);
    readback->read(frame.pixels.data(), frame.pixels.size());
    return frame;
}

// Counts the bytes of `frame` inside the integer rectangle that differ from `expected` (tightly
// packed, rect-sized), and the bytes outside it that are not the black clear.
struct Comparison {
    std::size_t inside_different = 0;
    std::size_t outside_not_clear = 0;
};

[[nodiscard]] Comparison compare(const Frame& frame, std::uint32_t x0, std::uint32_t y0,
                                 std::uint32_t width, std::uint32_t height,
                                 const std::vector<std::uint8_t>& expected) {
    Comparison result;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * frame.width + x) * 4U;
            const bool inside = x >= x0 && x < x0 + width && y >= y0 && y < y0 + height;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const std::uint8_t actual = frame.pixels[index + channel];
                if (inside) {
                    const std::size_t e =
                        (static_cast<std::size_t>(y - y0) * width + (x - x0)) * 4U + channel;
                    result.inside_different += actual != expected[e] ? 1U : 0U;
                } else {
                    result.outside_not_clear += actual != (channel == 3U ? 255U : 0U) ? 1U : 0U;
                }
            }
        }
    }
    return result;
}

struct Case {
    const char* name;
    std::uint32_t source_width;
    std::uint32_t source_height;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t x;
    std::uint32_t y;
};

bool run(rhi::Device& device) {
    // The same ratios as the measurement tool, plus an offset origin and the 4K -> 1080p case.
    const Case cases[] = {
        {"1:1", 240, 144, 240, 144, 0, 0},
        {"2:1 (every sample on a texel boundary)", 240, 144, 120, 72, 0, 0},
        {"3:1", 240, 144, 80, 48, 0, 0},
        {"3:2", 240, 144, 160, 96, 0, 0},
        {"1:2 upscale", 240, 144, 480, 288, 0, 0},
        {"fractional 240x144 -> 173x97", 240, 144, 173, 97, 0, 0},
        {"fractional 240x144 -> 199x131", 240, 144, 199, 131, 0, 0},
        {"2:1 at an offset origin", 240, 144, 120, 72, 7, 5},
        {"4K -> 1080p", 3840, 2160, 1920, 1080, 0, 0},
    };
    bool ok = true;
    for (const Case& c : cases) {
        const auto source = noise(c.source_width, c.source_height);
        rhi::TextureDesc desc;
        desc.extent = {c.source_width, c.source_height};
        desc.pixels = source.data();
        desc.row_pitch = static_cast<std::size_t>(c.source_width) * 4U;
        // Linear on purpose: the exact path must ignore the texture's own sampler state.
        desc.filter = rhi::Filter::Linear;
        auto texture = device.create_texture(desc);
        device.wait_idle();

        const std::uint32_t target_width = c.x + c.width + (c.x != 0 ? 9U : 0U);
        const std::uint32_t target_height = c.y + c.height + (c.y != 0 ? 4U : 0U);
        const Frame frame =
            draw_exact(device, *texture, target_width, target_height,
                       rhi::Rect{static_cast<float>(c.x), static_cast<float>(c.y),
                                 static_cast<float>(c.width), static_cast<float>(c.height)});
        const auto expected =
            cpu_nearest(source, c.source_width, c.source_height, c.width, c.height);
        const Comparison result = compare(frame, c.x, c.y, c.width, c.height, expected);
        const std::string label = std::string("exact nearest, ") + c.name;
        ok = expect(result.inside_different == 0,
                    (label + ": bytes differ from the CPU rule").c_str()) &&
             ok;
        ok = expect(result.outside_not_clear == 0,
                    (label + ": pixels outside the quad were touched").c_str()) &&
             ok;
        if (result.inside_different != 0 || result.outside_not_clear != 0) {
            std::cerr << "texture_exact: " << c.name << ": " << result.inside_different
                      << " differing bytes inside, " << result.outside_not_clear
                      << " touched outside\n";
        }
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
        std::cout << "texture_exact: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }
    if (!run(*device)) {
        std::cerr << "texture_exact: FAILED\n";
        return 1;
    }
    std::cout << "texture_exact: ok\n";
    return 0;
}
