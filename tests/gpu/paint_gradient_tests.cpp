// SPDX-License-Identifier: MIT
//
// GPU/CPU gradient parity and integration: rounded border-radius clipping, full-paint vs
// damage-clipped replay (no seam), and encoded-space ramp agreement with the deterministic CPU
// sink. Skips cleanly without a Vulkan device; fails on any validation message.

#include "cpu_paint_sink.hpp"
#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::poc::batched2d::CpuPaintSink;
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
using tessera::rendering::software::CpuCanvas;
using tessera::ui::LinearGradientStop;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr Extent2D kExtent{240, 160};
constexpr std::uint32_t kNodeX = 8;
constexpr std::uint32_t kNodeY = 8;
constexpr std::uint32_t kNodeW = 200;
constexpr std::uint32_t kNodeH = 120;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_gradient_gpu: " << message << "\n";
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

ComputedStyle gradient_style(float radius) {
    ComputedStyle style;
    style.set("color", "#FFFFFF");
    if (radius > 0.0f) {
        for (const char* property : {"border-top-left-radius", "border-top-right-radius",
                                     "border-bottom-right-radius", "border-bottom-left-radius"}) {
            style.set(property, std::to_string(radius) + "px");
        }
    }
    return style;
}

PaintData gradient_payload() {
    PaintData data;
    data.text = "";
    data.gradient = tessera::ui::LinearGradient{};
    data.gradient->angle_degrees = 180.0f; // top -> bottom
    data.gradient->stop_count = 2;
    data.gradient->stops[0] = LinearGradientStop{0.0f, rgba8(0, 0, 255)};
    data.gradient->stops[1] = LinearGradientStop{1.0f, rgba8(255, 0, 0)};
    return data;
}

RenderTargetDesc target_desc(LoadOp load = LoadOp::Clear) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = TextureFormat::R8G8B8A8Unorm;
    desc.load_op = load;
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

std::array<std::uint8_t, 4> pixel(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                  std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kExtent.width + x) * 4;
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

int channel_delta(const std::array<std::uint8_t, 4>& a, const std::array<std::uint8_t, 4>& b) {
    return std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) +
           std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1])) +
           std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
}

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "paint_gradient_gpu: skipped (Vulkan device unavailable: " << error.what()
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
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, true, false});
    Batched2DSink sink(renderer);

    Node root(1);
    root.set_bounds({0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
    Node* node = &root.add_child(std::make_unique<Node>(2));
    node->set_bounds({static_cast<float>(kNodeX), static_cast<float>(kNodeY),
                      static_cast<float>(kNodeW), static_cast<float>(kNodeH)});
    node->set_paint_data(gradient_payload());
    StubStyles styles;
    styles.set(gradient_style(0.0f));

    // Full render and a damage-clipped replay covering the whole width (two regions, seam in the
    // middle): the replay must be byte-identical to the full paint.
    std::vector<std::uint8_t> full;
    {
        auto target = device->create_render_target(target_desc());
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, target_desc());
        paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        full = read_target(*device, *target);
    }
    std::vector<std::uint8_t> damaged;
    {
        auto target = device->create_render_target(target_desc());
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, target_desc());
        paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        tessera::rendering::DamageAccumulator damage({kExtent, 16});
        damage.add(
            {0, 0, static_cast<float>(kExtent.width) / 2.0f, static_cast<float>(kExtent.height)});
        damage.add({static_cast<float>(kExtent.width) / 2.0f, 0,
                    static_cast<float>(kExtent.width) / 2.0f, static_cast<float>(kExtent.height)});
        const tessera::rendering::RedrawPlan plan = tessera::rendering::make_redraw_plan(damage);
        auto replay_commands = device->create_command_list();
        renderer.begin_damaged(*replay_commands, *target, target_desc(LoadOp::Load), plan);
        paint_scene(root, styles, sink);
        renderer.end();
        replay_commands->submit();
        device->wait_idle();
        damaged = read_target(*device, *target);
    }
    bool ok = expect(full == damaged, "damage-clipped gradient replay has no seam");

    // Rounded corners: the corner pixel outside the border radius stays the clear colour, while the
    // top and bottom interior pixels show the two gradient ends.
    StubStyles rounded_styles;
    rounded_styles.set(gradient_style(24.0f));
    std::vector<std::uint8_t> rounded;
    {
        auto target = device->create_render_target(target_desc());
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, target_desc());
        paint_scene(root, rounded_styles, sink);
        renderer.end();
        commands->submit();
        device->wait_idle();
        rounded = read_target(*device, *target);
    }
    const auto corner = pixel(rounded, kNodeX, kNodeY);
    const auto top = pixel(rounded, kNodeX + kNodeW / 2, kNodeY + 4);
    const auto bottom = pixel(rounded, kNodeX + kNodeW / 2, kNodeY + kNodeH - 4);
    ok = expect(corner[0] == 0 && corner[1] == 0 && corner[2] == 0,
                "rounded corner stays outside the gradient") &&
         ok;
    ok = expect(top[2] > top[0] && bottom[0] > bottom[2], "gradient runs top-blue to bottom-red") &&
         ok;

    // CPU parity: the deterministic CPU sink uses the same bands/ramp; interior pixels agree within
    // a documented 2/255 per-channel slack (GPU float blend vs CPU integer rounding).
    CpuCanvas canvas(kExtent.width, kExtent.height);
    canvas.clear(rgba8(0, 0, 0));
    CpuPaintSink cpu_sink(canvas);
    paint_scene(root, styles, cpu_sink);
    const auto cpu_mid =
        pixel(std::vector<std::uint8_t>(canvas.pixels().begin(), canvas.pixels().end()),
              kNodeX + kNodeW / 2, kNodeY + kNodeH / 2);
    const auto gpu_mid = pixel(full, kNodeX + kNodeW / 2, kNodeY + kNodeH / 2);
    ok =
        expect(channel_delta(cpu_mid, gpu_mid) <= 6, "CPU/GPU gradient interior parity (<=2/ch)") &&
        ok;

    if (!ok) {
        std::cerr << "paint_gradient_gpu: failed\n";
        return 1;
    }
    std::cout << "paint_gradient_gpu: ok\n";
    return 0;
}
