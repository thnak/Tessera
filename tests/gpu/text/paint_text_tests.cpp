// SPDX-License-Identifier: MIT
//
// Real-Vulkan paint-text readback: a node's PaintData text is painted through
// paint_scene -> Batched2DSink -> TextRenderer/glyph atlas -> Batched2DRenderer, clipped by the
// paint clip stack. Asserts glyph coverage inside the text box, untouched background outside it,
// deterministic repeat, and that an sRGB target renders through the same path.
// Skips cleanly without a Vulkan device or the test font fixture.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
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
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextRenderer;

constexpr Extent2D kExtent{160, 160};
constexpr std::uint32_t kTextRectX = 8;
constexpr std::uint32_t kTextRectY = 8;
constexpr std::uint32_t kTextRectW = 112;
constexpr std::uint32_t kTextRectH = 40;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
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

std::size_t count_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t count_non_background_region(const std::vector<std::uint8_t>& pixels, std::uint32_t x0,
                                        std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    std::size_t count = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t count_far_right_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = kExtent.width - 4; x < kExtent.width; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    if (argc < 2) {
        std::cout << "paint_text_gpu: skipped (missing font fixture path)\n";
        return 0;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cout << "paint_text_gpu: skipped (cannot load fixture: " << loaded.error().message
                  << ")\n";
        return 0;
    }

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "paint_text_gpu: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text_renderer(*device, rasterizer, {128, 128, 2, 1});

    auto pipeline = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Unorm, 1));

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});
    Batched2DSink sink(renderer, &text_renderer, &shaper);

    auto pipeline_srgb = device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(TextureFormat::R8G8B8A8Srgb, 1));
    Batched2DRenderer renderer_srgb(*device, *pipeline_srgb, *white,
                                    Batched2DConfig{4096, false, true});
    Batched2DSink sink_srgb(renderer_srgb, &text_renderer, &shaper);

    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"}, {"color", "white"}}));
    Node root(1);
    root.set_bounds({static_cast<float>(kTextRectX), static_cast<float>(kTextRectY),
                     static_cast<float>(kTextRectW), static_cast<float>(kTextRectH)});
    PaintData data;
    data.text = "Tessera";
    data.text_style.font_size = 24.0f;
    root.set_paint_data(std::move(data));

    Node wrapped_root(2);
    wrapped_root.set_bounds({8, 8, 96, 120});
    PaintData wrapped_data;
    wrapped_data.text = "The quick brown fox jumps over the lazy dog near the river bank at dawn";
    wrapped_data.wrap = tessera::ui::PaintWrap::Wrap;
    wrapped_data.text_style.font_size = 16.0f;
    wrapped_root.set_paint_data(std::move(wrapped_data));
    styles.set(2, styled({{"background-color", "black"},
                          {"color", "white"},
                          {"font-size", "16px"},
                          {"padding-left", "4px"},
                          {"padding-right", "4px"},
                          {"padding-top", "4px"},
                          {"padding-bottom", "4px"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"}}));

    const auto render_frame = [&](Batched2DRenderer& frame_renderer, Batched2DSink& frame_sink,
                                  Node& scene, RenderTarget& target, const RenderTargetDesc& desc) {
        auto commands = device->create_command_list();
        frame_renderer.begin(*commands, target, desc);
        frame_sink.begin_text_frame();
        paint_scene(scene, styles, frame_sink);
        frame_renderer.end();
        commands->submit();
        device->wait_idle();
        return read_target(*device, target);
    };

    const RenderTargetDesc desc1 = target_desc(TextureFormat::R8G8B8A8Unorm);
    auto target1 = device->create_render_target(desc1);
    const std::vector<std::uint8_t> first = render_frame(renderer, sink, root, *target1, desc1);
    const std::size_t coverage = count_non_background(first);
    bool ok = expect(coverage > 0, "text paints pixels inside its rect") &&
              expect(count_far_right_non_background(first) == 0,
                     "pixels outside the text rect stay background");

    const std::vector<std::uint8_t> repeat = render_frame(renderer, sink, root, *target1, desc1);
    ok = expect(first == repeat, "paint-text render is byte-identical on repeat") && ok;

    // sRGB target path: same paint path, colors encoded by the target format.
    const RenderTargetDesc desc_srgb = target_desc(TextureFormat::R8G8B8A8Srgb);
    auto target_srgb = device->create_render_target(desc_srgb);
    const std::vector<std::uint8_t> srgb =
        render_frame(renderer_srgb, sink_srgb, root, *target_srgb, desc_srgb);
    ok = expect(count_non_background(srgb) > 0, "text paints on an sRGB target") && ok;

    // Wrapped paragraph: multiple line bands inside the padded content box, background outside it,
    // deterministic repeat, and the same path on an sRGB target.
    const std::vector<std::uint8_t> wrapped =
        render_frame(renderer, sink, wrapped_root, *target1, desc1);
    const std::size_t first_band = count_non_background_region(wrapped, 12, 12, 100, 32);
    const std::size_t second_band = count_non_background_region(wrapped, 12, 32, 100, 52);
    const std::size_t third_band = count_non_background_region(wrapped, 12, 52, 100, 72);
    ok = expect(first_band > 0 && second_band > 0 && third_band > 0,
                "wrapped paragraph paints several line bands") &&
         ok;
    ok = expect(count_far_right_non_background(wrapped) == 0,
                "wrapped text stays inside its content box") &&
         ok;
    ok = expect(count_non_background_region(wrapped, 0, 140, kExtent.width, kExtent.height) == 0,
                "wrapped text stays above the node bottom") &&
         ok;

    const std::vector<std::uint8_t> wrapped_repeat =
        render_frame(renderer, sink, wrapped_root, *target1, desc1);
    ok = expect(wrapped == wrapped_repeat, "wrapped paragraph is byte-identical on repeat") && ok;

    const std::vector<std::uint8_t> wrapped_srgb =
        render_frame(renderer_srgb, sink_srgb, wrapped_root, *target_srgb, desc_srgb);
    ok = expect(count_non_background(wrapped_srgb) > 0,
                "wrapped paragraph paints on an sRGB target") &&
         ok;

    if (!ok) {
        std::cerr << "paint_text_gpu: failed\n";
        return 1;
    }
    std::cout << "paint_text_gpu: ok (coverage=" << coverage << ")\n";
    return 0;
}
