// SPDX-License-Identifier: MIT
//
// Real-GPU bidi paint readback: a Hebrew PaintData run is laid out through the bidi line-box path
// and painted as visual runs (RTL glyphs) with a selection + caret. Asserts painted pixels inside
// the node, background outside, and a byte-identical repeat. The deterministic bidi-capable test
// font is NotoSansHebrew-Regular.ttf (OFL-1.1, test-only). Skips cleanly without a Vulkan device or
// the fixture, and fails on any validation message.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::TextDecoration;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::FontPaintTextMetrics;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontFace;

constexpr Extent2D kExtent{240, 96};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_bidi_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(ComputedStyle style) {
        style_ = std::move(style);
    }
    ComputedStyle compute(const Node&) override {
        return style_;
    }

private:
    ComputedStyle style_;
};

ComputedStyle styled() {
    ComputedStyle style;
    style.set("background-color", "#000000");
    style.set("color", "#FFFFFF");
    style.set("font-size", "28px");
    return style;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = TextureFormat::R8G8B8A8Unorm;
    desc.load_op = LoadOp::Clear;
    desc.clear_color = rgba8(0, 0, 0);
    return desc;
}

std::vector<std::uint8_t> read_target(Device& device,
                                      tessera::rendering::rhi::RenderTarget& target) {
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

std::size_t count_ink(const std::vector<std::uint8_t>& pixels, std::uint32_t x0, std::uint32_t y0,
                      std::uint32_t x1, std::uint32_t y1) {
    std::size_t count = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * kExtent.width + x) * 4;
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "paint_bidi_gpu: skipped (missing bidi font fixture)\n";
        return 0;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cout << "paint_bidi_gpu: skipped (cannot load fixture: " << loaded.error().message
                  << ")\n";
        return 0;
    }
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "paint_bidi_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    auto pipeline = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    tessera::ui::text::GlyphRasterizer rasterizer(*loaded);
    tessera::ui::text::HarfBuzzShaper shaper(*loaded);
    tessera::ui::text::TextRenderer text_renderer(*device, rasterizer, {256, 256, 2, 1});
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});
    Batched2DSink sink(renderer, &text_renderer, &shaper);
    FontPaintTextMetrics metrics(*loaded, 7);

    Node root(1);
    root.set_bounds({0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
    Node* text = &root.add_child(std::make_unique<Node>(2));
    text->set_bounds({8, 8, 200, 72});
    PaintData payload;
    payload.text = "\xD7\x90\xD7\x91\xD7\x92\xD7\x93\xD7\x94\xD7\x95"; // Hebrew
    payload.text_style.font_size = 28.0f;
    payload.text_style.color = rgba8(255, 255, 255);
    TextDecoration decoration;
    decoration.caret_byte_offset = 0;
    decoration.selection_begin = 0;
    decoration.selection_end = 4;
    payload.decoration = decoration;
    text->set_paint_data(std::move(payload));

    StubStyles styles;
    styles.set(styled());
    PaintContext context;
    context.text_metrics_source = &metrics;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> repeat;
    for (std::vector<std::uint8_t>* output : {&first, &repeat}) {
        auto target = device->create_render_target(target_desc());
        auto commands = device->create_command_list();
        const RenderTargetDesc desc = target_desc();
        renderer.begin(*commands, *target, desc);
        sink.begin_text_frame();
        paint_scene(root, styles, sink, context);
        renderer.end();
        commands->submit();
        device->wait_idle();
        *output = read_target(*device, *target);
    }

    const std::size_t ink = count_ink(first, 8, 8, 208, 80);
    bool ok = expect(ink > 0, "RTL visual runs paint inside the node") &&
              expect(count_ink(first, 208, 0, kExtent.width, kExtent.height) == 0,
                     "bidi paint stays inside the node") &&
              expect(first == repeat, "bidi paint is byte-identical on repeat");
    if (!ok) {
        std::cerr << "paint_bidi_gpu: failed\n";
        return 1;
    }
    std::cout << "paint_bidi_gpu: ok (ink=" << ink << ")\n";
    return 0;
}
