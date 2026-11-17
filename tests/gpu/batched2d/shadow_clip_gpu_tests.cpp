// SPDX-License-Identifier: MIT
//
// Real-Vulkan pixel proof of issue #461: an outer `box-shadow` is painted only outside the node's
// border box (CSS Backgrounds 3 section 7.1.1), rounded corners included. A transparent box with a
// shadow must leave its whole border box at the page colour - the centre, the interior edges and
// the corner triangles of a rounded box - while the shadow still paints around it. Deterministic:
// UNORM target, AA off (golden config), exact byte comparison. Runs on lavapipe in CI and on a real
// GPU; the committed image golden for the same case is `batched2d_shadow_golden` (lavapipe).

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::ui::Node;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr Extent2D kExtent{96, 96};
constexpr Color kBackground = rgba8(255, 255, 255);
constexpr Color kShadow = rgba8(0, 0, 0, 77); // rgba(0, 0, 0, 0.3)
// The node every case paints, and the same box moved down by the 2 px shadow offset.
constexpr Rect kNode{24, 24, 48, 48};
constexpr Rect kShadowBox{24, 26, 48, 48};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "shadow_clip: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = kBackground;
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

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool is_page(const std::array<std::uint8_t, 4>& pixel) {
    return pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255 && pixel[3] == 255;
}

// Any grey the shadow would leave behind: all channels below the page colour by more than rounding.
bool is_shaded(const std::array<std::uint8_t, 4>& pixel) {
    return pixel[0] < 250 && pixel[1] < 250 && pixel[2] < 250;
}

// A style provider that hands every node the same computed style (the whole page is one node).
class StubStyles final : public StyleProvider {
public:
    explicit StubStyles(ComputedStyle style)
        : style_(std::move(style)) {}
    ComputedStyle compute(const Node&) override {
        return style_;
    }

private:
    ComputedStyle style_;
};

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "shadow_clip: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    const RenderTargetDesc desc = target_desc();
    auto target = device->create_render_target(desc);
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    auto pipeline = device->create_pipeline(pipeline_desc);

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    // AA off + golden mode: hard edges and exact bytes.
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});

    bool ok = true;
    {
        // The square case, straight from the renderer: one Gaussian quad, carved by the node box.
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        renderer.push_soft_shadow(kShadowBox, kNode, kShadow, 0.0f, 4.0f);
        renderer.end();
        commands->submit();
        device->wait_idle();
        const auto pixels = read_target(*device, *target);
        ok = expect(is_page(pixel_at(pixels, 48, 48)),
                    "the centre of a transparent shadowed box is the page background") &&
             expect(is_page(pixel_at(pixels, 48, 70)) && is_page(pixel_at(pixels, 25, 25)) &&
                        is_page(pixel_at(pixels, 71, 71)),
                    "the whole border box stays at the page background") &&
             expect(is_shaded(pixel_at(pixels, 48, 78)),
                    "the blurred shadow still paints below the box") &&
             expect(is_page(pixel_at(pixels, 48, 8)) && is_page(pixel_at(pixels, 8, 48)),
                    "nothing paints beyond the shadow's reach") &&
             ok;
    }
    {
        // The rounded case. Chrome clips the shadow inside the *rounded* border box, so the
        // interior near a corner is the page colour while the corner triangle (inside the box
        // rectangle, outside the arc) keeps the shadow the silhouette casts there: Chrome paints
        // 244 there for this geometry and a rectangular carve would wrongly leave the page colour.
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        renderer.push_soft_shadow(kShadowBox, kNode, kShadow, 12.0f, 4.0f);
        renderer.end();
        commands->submit();
        device->wait_idle();
        const auto pixels = read_target(*device, *target);
        const auto corner = pixel_at(pixels, 27, 27);
        ok = expect(is_page(pixel_at(pixels, 48, 48)) && is_page(pixel_at(pixels, 31, 31)) &&
                        is_page(pixel_at(pixels, 48, 70)),
                    "the rounded box's interior is the page background, corners included") &&
             expect(corner[0] > 200 && corner[0] < 254 && corner[1] == corner[0] &&
                        corner[2] == corner[0],
                    "the corner triangle keeps a light shadow (rounded carve, not a rect)") &&
             expect(is_shaded(pixel_at(pixels, 48, 78)),
                    "the rounded shadow still paints below the box") &&
             ok;
    }
    {
        // The sharp path (sigma 0) through the real paint pass: a transparent node with an offset,
        // unblurred shadow keeps its overlap with the shadow at the page colour.
        Node node(1);
        node.set_bounds(kNode);
        ComputedStyle style;
        style.set("box-shadow", "8px 8px 0px rgba(0, 0, 0, 0.6)");
        StubStyles styles(style);
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        Batched2DSink sink(renderer);
        paint_scene(node, styles, sink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        const auto pixels = read_target(*device, *target);
        ok = expect(is_page(pixel_at(pixels, 48, 48)) && is_page(pixel_at(pixels, 68, 68)),
                    "a sharp shadow is carved out of the whole border box") &&
             expect(is_shaded(pixel_at(pixels, 76, 76)),
                    "the sharp shadow still paints outside the box") &&
             ok;
    }
    {
        // The blurred path through the real paint pass, with the acceptance's own declaration.
        Node node(2);
        node.set_bounds(kNode);
        ComputedStyle style;
        style.set("box-shadow", "0px 2px 8px rgba(0, 0, 0, 0.3)");
        StubStyles styles(style);
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, desc);
        Batched2DSink sink(renderer);
        paint_scene(node, styles, sink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        const auto pixels = read_target(*device, *target);
        ok = expect(is_page(pixel_at(pixels, 48, 48)),
                    "the paint pass leaves the centre of a transparent blurred box at the page") &&
             expect(is_shaded(pixel_at(pixels, 48, 78)),
                    "the paint pass still paints the blurred shadow below it") &&
             ok;
    }

    if (!ok) {
        std::cerr << "shadow_clip: failed\n";
        return 1;
    }
    std::cout << "shadow_clip: ok\n";
    return 0;
}
