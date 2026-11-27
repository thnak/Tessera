// SPDX-License-Identifier: MIT
//
// The CSS colour filter functions (issue #617, docs/design/gpu/offscreen-layers.md section 5.2) on
// a real device, through the real path: `filter: <css>` on a node -> style parse -> paint pass ->
// Batched2DSink -> the glass_blur.frag mode 3 pass over the layer's pooled target.
//
// Evidence:
//  - every sample of tests/fixtures/chromium_color_filter.hpp (opaque colours, and translucent ones
//    over black, which is the premultiplied colour) is reproduced within 4/255: the shader works on
//    the straight-alpha colour of a premultiplied layer and clamps after every function;
//  - a colour function chained with a blur runs in order, both counted as applied;
//  - without the blur program the node paints unfiltered and the function is counted, never lost
//    silently;
//  - the run is validation-clean.
//
// Skips cleanly when the machine has no Vulkan device.

#include "chromium_color_filter.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/filter_value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace chromium = tessera::fixtures::chromium_filter;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 4; // out of 255: the 8-bit premultiplied layer, divided back by alpha
constexpr rhi::Rect kBox{24.0f, 24.0f, 16.0f, 16.0f};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "layer_color_filter_gpu: " << message << "\n";
    }
    return condition;
}

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
    std::unique_ptr<rhi::Pipeline> mask;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

using Pixels = std::vector<std::uint8_t>;

Pixels read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    Pixels pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

int channel(const Pixels& pixels, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return pixels[(static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t) + c];
}

class StubStyles final : public tessera::ui::paint::StyleProvider {
public:
    void set(tessera::ui::NodeId id, tessera::ui::style::ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    tessera::ui::style::ComputedStyle compute(const tessera::ui::Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? tessera::ui::style::ComputedStyle{} : found->second;
    }

private:
    std::map<tessera::ui::NodeId, tessera::ui::style::ComputedStyle> styles_;
};

void clear_tree(tessera::ui::Node& node) {
    node.clear_dirty(tessera::ui::Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

struct Frame {
    Pixels pixels;
    std::uint32_t applied = 0;
    std::uint32_t unsupported = 0;
};

// A 16x16 node at (24,24) with `background-color` and `filter: <css>`, painted through the real
// paint path onto black.
Frame render(Harness& harness, const std::string& background, const std::string& css,
             bool with_blur = true) {
    using tessera::ui::Node;
    using tessera::ui::style::ComputedStyle;
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 64.0f, 64.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds(kBox);
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    style.set("background-color", background);
    style.set("filter", css);
    styles.set(2, style);
    clear_tree(root);

    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   GlassPipelines{with_blur ? harness.blur.get() : nullptr,
                                                  harness.composite.get(), harness.mask.get()});
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        frame.applied = renderer.layer_filters_applied();
        frame.unsupported = renderer.layer_filters_unsupported();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

std::string rgba_text(const std::array<int, 3>& value, float alpha) {
    return "rgba(" + std::to_string(value[0]) + "," + std::to_string(value[1]) + "," +
           std::to_string(value[2]) + "," + std::to_string(alpha) + ")";
}

bool near(const Pixels& pixels, std::uint32_t x, std::uint32_t y, const std::array<int, 3>& want,
          const std::string& what) {
    for (std::size_t c = 0; c < 3; ++c) {
        if (std::abs(channel(pixels, x, y, c) - want[c]) > kTolerance) {
            std::cerr << "layer_color_filter_gpu: " << what << " at (" << x << "," << y
                      << "): got (" << channel(pixels, x, y, 0) << "," << channel(pixels, x, y, 1)
                      << "," << channel(pixels, x, y, 2) << "), expected (" << want[0] << ","
                      << want[1] << "," << want[2] << ")\n";
            return false;
        }
    }
    return true;
}

bool matches_chromium(Harness& harness) {
    bool ok = true;
    for (const chromium::OpaqueSample& sample : chromium::kOpaque) {
        const Frame frame = render(harness, rgba_text(sample.input, 1.0f), sample.css);
        // An identity (brightness(), hue-rotate(1turn), ...) parses to nothing and paints no layer.
        const std::uint32_t want_applied =
            tessera::ui::style::parse_filter(sample.css).filter.none() ? 0u : 1u;
        ok = expect(frame.applied == want_applied && frame.unsupported == 0,
                    std::string("applied: ") + sample.css) &&
             ok;
        ok = near(frame.pixels, 32, 32, sample.output, std::string(sample.css) + " (opaque)") && ok;
    }
    for (const chromium::AlphaSample& sample : chromium::kAlpha) {
        const Frame frame = render(harness, rgba_text(sample.input, sample.alpha), sample.css);
        ok = near(frame.pixels, 32, 32, sample.over_black,
                  std::string(sample.css) + " on " + rgba_text(sample.input, sample.alpha)) &&
             ok;
    }
    return ok;
}

bool chained_with_a_blur(Harness& harness) {
    // Inverted red is cyan; the blur (sigma 1) leaves the middle of a 16 px box untouched and
    // softens the edge, where the order shows: invert first, then blur, never a blurred red
    // inverted into a bright fringe outside the box.
    const Frame frame = render(harness, "rgb(255,0,0)", "invert(1) blur(1px)");
    bool ok = expect(frame.applied == 2 && frame.unsupported == 0, "both functions applied");
    ok = near(frame.pixels, 32, 32, {0, 255, 255}, "the middle is the inverted colour") && ok;
    ok = near(frame.pixels, 20, 32, {0, 0, 0}, "far outside the box stays the page") && ok;
    const Frame reversed = render(harness, "rgb(255,0,0)", "blur(1px) invert(1)");
    // Inverting after the blur turns the soft edge's partly covered pixels into cyan at the same
    // alpha, so the fringe pixel is darker cyan, not a mix of the two: green equals blue.
    ok = expect(channel(reversed.pixels, 23, 32, 1) == channel(reversed.pixels, 23, 32, 2),
                "blur then invert keeps a cyan fringe") &&
         ok;
    return ok;
}

bool without_the_program(Harness& harness) {
    const Frame frame = render(harness, "rgb(255,0,0)", "invert(1)", false);
    bool ok = expect(frame.applied == 0 && frame.unsupported == 1,
                     "without the blur program the function is counted, not applied");
    ok = near(frame.pixels, 32, 32, {255, 0, 0}, "the node still paints, unfiltered") && ok;
    return ok;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;

    Harness harness;
    try {
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "layer_color_filter_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    harness.pipeline = harness.device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(format));
    harness.blur = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    harness.composite = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
    harness.mask =
        harness.device->create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc(
            format, rhi::BlendMode::DestinationIn));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = matches_chromium(harness);
    ok = chained_with_a_blur(harness) && ok;
    ok = without_the_program(harness) && ok;

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_color_filter_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_color_filter_gpu: ok\n";
    return 0;
}
