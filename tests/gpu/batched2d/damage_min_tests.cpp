// SPDX-License-Identifier: MIT
//
// Minimal damage reproduction: one solid node on a plain background. Frame 1 full redraw; frame 2
// moves the node by (8,8) and renders through begin_damaged with the union of old+new bounds. The
// cumulative target is compared byte-for-byte with a fresh full redraw of the final scene, and the
// differing pixels are classified (inside/outside the damage rect). Runs with 0 and 2 px margin.

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/redraw_plan.hpp"
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
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr Extent2D kExtent{64, 64};

class MapStyles final : public StyleProvider {
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

ComputedStyle style(const char* property, const char* value) {
    ComputedStyle result;
    result.set(property, value);
    return result;
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

RenderTargetDesc target_desc(LoadOp load_op) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = TextureFormat::R8G8B8A8Unorm;
    desc.load_op = load_op;
    desc.clear_color = rgba8(11, 18, 32);
    return desc;
}

struct Scene {
    Node root{1};
    Node* box = nullptr;
    MapStyles styles;
    Scene() {
        root.set_bounds({0, 0, 64, 64});
        box = &root.add_child(std::make_unique<Node>(2));
        box->set_bounds({8, 8, 24, 24});
        styles.set(1, style("background-color", "#0B1220"));
        styles.set(2, style("background-color", "#EF4444"));
    }
};

} // namespace

int main() {
    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;
    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "damage_min: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});
    Batched2DSink sink(renderer);

    auto target = device->create_render_target(target_desc(LoadOp::Clear));
    Scene scene;

    const auto paint_frame = [&](const RedrawPlan& plan, bool clear) {
        auto commands = device->create_command_list();
        RenderTargetDesc desc = target_desc(clear ? LoadOp::Clear : LoadOp::Load);
        if (clear || plan.full_redraw) {
            desc.load_op = LoadOp::Clear;
            renderer.begin(*commands, *target, desc);
        } else {
            renderer.begin_damaged(*commands, *target, desc, plan);
        }
        paint_scene(scene.root, scene.styles, sink);
        renderer.end();
        commands->submit();
    };

    paint_frame(RedrawPlan{true, {}}, true); // frame 1: full
    scene.box->set_bounds({16, 16, 24, 24});
    RedrawPlan plan;
    for (const float margin : {0.0f, 2.0f}) {
        DamageAccumulator damage(DamageConfig{kExtent, 8});
        damage.add({8, 8, 32, 32}, margin);
        plan = make_redraw_plan(damage);
        // Re-render frame 1 cleanly for each margin experiment.
        paint_frame(RedrawPlan{true, {}}, true);
        paint_frame(plan, false);
        const auto damaged = read_target(*device, *target);

        auto reference_target = device->create_render_target(target_desc(LoadOp::Clear));
        auto commands = device->create_command_list();
        const auto desc = target_desc(LoadOp::Clear);
        renderer.begin(*commands, *reference_target, desc);
        paint_scene(scene.root, scene.styles, sink);
        renderer.end();
        commands->submit();
        const auto reference = read_target(*device, *reference_target);

        std::size_t diff = 0;
        std::size_t diff_inside = 0;
        std::size_t diff_outside = 0;
        std::size_t first_x = 0;
        std::size_t first_y = 0;
        for (std::size_t i = 0; i + 3 < damaged.size(); i += 4) {
            if (damaged[i] == reference[i] && damaged[i + 1] == reference[i + 1] &&
                damaged[i + 2] == reference[i + 2] && damaged[i + 3] == reference[i + 3]) {
                continue;
            }
            const std::size_t pixel = i / 4;
            const std::size_t x = pixel % kExtent.width;
            const std::size_t y = pixel / kExtent.width;
            if (diff == 0) {
                first_x = x;
                first_y = y;
            }
            ++diff;
            const bool inside = x >= 8 && x < 40 && y >= 8 && y < 40;
            (inside ? diff_inside : diff_outside) += 1;
        }
        std::cout << "damage_min margin=" << margin << " full=" << plan.full_redraw
                  << " regions=" << plan.scissors.size();
        if (!plan.scissors.empty()) {
            std::cout << " scissor=" << plan.scissors[0].x << "," << plan.scissors[0].y << ","
                      << plan.scissors[0].width << "x" << plan.scissors[0].height;
        }
        std::cout << " diff=" << diff << " inside=" << diff_inside << " outside=" << diff_outside
                  << " first=" << first_x << "," << first_y << "\n";
    }
    return 0;
}
