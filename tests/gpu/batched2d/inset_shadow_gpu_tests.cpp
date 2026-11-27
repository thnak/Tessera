// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel proof of the inset `box-shadow` (issue #612): the batched 2D fragment shader's
// inset branch (vertex mode 2048 + sigma) against Chromium. A white box carries one inset shadow
// and is painted through the real paint pass and Batched2DSink; 69 samples per scene (row, column
// and diagonal, inset_shadow_chromium_reference.hpp) are compared with what Chromium drew.
//
// The branch was unreachable for as long as the shader tested `mode > 1000` (the outer shadow,
// 1024 + sigma) before `mode > 2000`: an inset shadow of any blur took the outer-shadow path with a
// sigma over a thousand, which paints a flat half-strength ring. This test is what caught it.
//
// Skips cleanly when the machine has no Vulkan device.

#include "inset_shadow_chromium_reference.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace reference = inset_shadow_chromium_reference;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;

constexpr std::uint32_t kSize = 64;

// The most the shader may be from Chromium on a compared sample, out of 255. The shader integrates
// the Gaussian exactly along x and in eight midpoint steps along y; the reference was drawn by
// Skia's software blur, which approximates it, so a few levels are the two renderers' own.
constexpr int kBudget = 6;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "inset_shadow_gpu: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

class Styles final : public tessera::ui::paint::StyleProvider {
public:
    explicit Styles(tessera::ui::style::ComputedStyle style)
        : style_(std::move(style)) {}
    tessera::ui::style::ComputedStyle compute(const tessera::ui::Node&) override {
        return style_;
    }

private:
    tessera::ui::style::ComputedStyle style_;
};

struct Comparison {
    int worst = 0;
    int compared = 0;
    int shaded = 0; // samples where Chromium drew something other than the white box
    std::string where;
    int got = 0;
    int expected = 0;
};

int channel_of(std::uint32_t packed, int channel) {
    return static_cast<int>((packed >> (16 - 8 * channel)) & 0xFFU);
}

// A sample on the silhouette's own edge: Chromium anti-aliases the rounded corner, the golden
// config does not, so a big step against a neighbour two pixels away is the edge, not the shadow.
bool is_edge(const std::array<std::uint32_t, reference::kSamples>& series, int index) {
    for (const int neighbour : {index - 1, index + 1}) {
        if (neighbour < 0 || neighbour >= reference::kSamples) {
            continue;
        }
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(channel_of(series[index], channel) -
                         channel_of(series[neighbour], channel)) > 80) {
                return true;
            }
        }
    }
    return false;
}

void compare_series(const std::vector<std::uint8_t>& pixels,
                    const std::array<std::uint32_t, reference::kSamples>& series, const char* line,
                    bool row, bool diagonal, Comparison& result) {
    for (int index = 0; index < reference::kSamples; ++index) {
        if (is_edge(series, index)) {
            continue;
        }
        const int at = 10 + 2 * index;
        const int x = row || diagonal ? at : 32;
        const int y = row ? 32 : at;
        const std::size_t offset =
            (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
        if (series[index] != 0xFFFFFFU) {
            ++result.shaded;
        }
        for (int channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(pixels[offset + channel]) -
                                       channel_of(series[index], channel));
            ++result.compared;
            if (delta > result.worst) {
                result.worst = delta;
                result.where = std::string(line) + " at " + std::to_string(at) + " channel " +
                               std::to_string(channel);
                result.got = pixels[offset + channel];
                result.expected = channel_of(series[index], channel);
            }
        }
    }
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
        std::cout << "inset_shadow_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    const rhi::RenderTargetDesc desc = target_desc();
    auto pipeline = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(rhi::TextureFormat::R8G8B8A8Unorm));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    bool ok = true;
    for (const reference::Scene& scene : reference::kScenes) {
        tessera::ui::Node node(1);
        node.set_bounds({8.0f, 8.0f, 48.0f, 48.0f});
        tessera::ui::style::ComputedStyle style;
        style.set("background-color", "#ffffff");
        style.set("box-shadow", scene.shadow);
        if (scene.radius > 0.0f) {
            const std::string radius = std::to_string(static_cast<int>(scene.radius)) + "px";
            for (const char* corner : {"border-top-left-radius", "border-top-right-radius",
                                       "border-bottom-right-radius", "border-bottom-left-radius"}) {
                style.set(corner, radius);
            }
        }
        Styles styles(style);
        node.clear_dirty(tessera::ui::Dirty::All);

        auto target = device->create_render_target(desc);
        auto commands = device->create_command_list();
        Batched2DConfig config;
        config.enable_aa = false;
        config.golden_mode = true;
        {
            Batched2DRenderer renderer(*device, *pipeline, *white, config);
            tessera::ui::paint::Batched2DSink sink(renderer);
            renderer.begin(*commands, *target, desc);
            (void)tessera::ui::paint::paint_scene(node, styles, sink);
            renderer.end();
            commands->submit();
        }
        const std::vector<std::uint8_t> pixels = read_target(*device, *target);
        Comparison result;
        compare_series(pixels, scene.row32, "row 32", true, false, result);
        compare_series(pixels, scene.col32, "column 32", false, false, result);
        compare_series(pixels, scene.diagonal, "diagonal", false, true, result);
        std::cout << "inset_shadow_gpu: " << scene.name << ": " << result.worst
                  << "/255 from Chromium (" << result.compared << " channel samples, "
                  << result.shaded << " samples with a shadow)\n";
        ok = expect(result.shaded >= 3,
                    std::string(scene.name) + ": the scene has a shadow to compare") &&
             ok;
        if (result.worst > kBudget) {
            std::cerr << "inset_shadow_gpu: " << scene.name << " is " << result.worst
                      << "/255 from Chromium at " << result.where << ": got " << result.got
                      << ", Chromium " << result.expected << " (budget " << kBudget << ")\n";
            ok = false;
        }
    }
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "inset_shadow_gpu: failed\n";
        return 1;
    }
    std::cout << "inset_shadow_gpu: ok\n";
    return 0;
}
