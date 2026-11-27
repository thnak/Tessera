// SPDX-License-Identifier: MIT
//
// Damage-limited redraw of a filtered node on a real device (issue #617,
// docs/design/gpu/offscreen-layers.md section 5d). A blur reads pixels up to its reach away, so a
// change inside a filtered node repaints pixels well outside the changed rectangle. The claim:
// after a child of a `filter: blur()` node moves, redrawing only what the frame pipeline's redraw
// plan names produces exactly the pixels of redrawing the whole frame. That holds only because the
// filter dependency walk (DamageTracker::add_filter_dependencies) widens the damage to the node's
// whole region; without it the halo around the child's old and new boxes keeps stale blur.
//
// Skips cleanly when the machine has no Vulkan device.

#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
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
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kSize = 128;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "filter_damage_gpu: " << message << "\n";
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
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
    desc.clear_color = rhi::rgba8(11, 18, 32);
    return desc;
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * 4;
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    device.wait_idle();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

// root(1) > card(2) with `filter: blur(2px)` (halo 6 px) > child(3), a red square. The card draws
// nothing of its own, so the only pixels are the child's blur.
struct Scene {
    Node root{1};
    StubStyles styles;

    Scene() {
        root.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
        auto card = std::make_unique<Node>(2);
        card->set_bounds({20.0f, 20.0f, 48.0f, 48.0f});
        auto child = std::make_unique<Node>(3);
        child->set_bounds({4.0f, 4.0f, 10.0f, 10.0f});
        card->add_child(std::move(child));
        root.add_child(std::move(card));
        styles.set(1, styled({{"background-color", "#0b1220"}}));
        styles.set(2, styled({{"filter", "blur(2px)"}}));
        styles.set(3, styled({{"background-color", "#ff0000"}}));
    }
    Node& child() {
        return *root.children()[0]->children()[0];
    }
};

struct Pipeline {
    ManualClock clock;
    RuntimeLoop loop{clock};
    AnimationRuntime animations;
    DamageAccumulator damage{DamageConfig{rhi::Extent2D{kSize, kSize}, 16}};
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline{animations, loop, tracker, damage, dirty, FramePipelineConfig{}};
};

// Paints `scene` into `target`, redrawing only `plan` when one is given.
void paint(Harness& harness, Scene& scene, rhi::RenderTarget& target,
           const tessera::rendering::RedrawPlan* plan) {
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.golden_mode = true;
    Batched2DRenderer renderer(
        *harness.device, *harness.pipeline, *harness.white, config,
        GlassPipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()});
    tessera::ui::paint::Batched2DSink sink(renderer);
    if (plan != nullptr) {
        renderer.begin_damaged(*commands, target, target_desc(), *plan);
    } else {
        renderer.begin(*commands, target, target_desc());
    }
    (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink);
    renderer.end();
    commands->submit();
    harness.device->wait_idle();
}

bool damaged_redraw_equals_full_redraw(Harness& harness) {
    Scene scene;
    Pipeline state;
    state.pipeline.sync_style(scene.root, scene.styles, state.pipeline.tick(scene.root, 1.0 / 60.0),
                              true);
    const FrameRender first = state.pipeline.finish_frame(scene.root);
    bool ok = expect(first.should_render, "the first frame renders");

    auto incremental = harness.device->create_render_target(target_desc());
    paint(harness, scene, *incremental, nullptr);
    clear_tree(scene.root);

    // Move the child inside the filtered card: its old and new boxes are the only dirty nodes, but
    // the blur around both changes too.
    scene.child().set_bounds({30.0f, 30.0f, 10.0f, 10.0f}); // world (50, 50)
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender second = state.pipeline.finish_frame(scene.root);
    ok = expect(second.should_render && !second.redraw_plan.full_redraw,
                "a partial redraw is planned") &&
         ok;
    ok = expect(second.filter_dependencies == 1, "the filtered node's region joined the damage") &&
         ok;
    paint(harness, scene, *incremental, &second.redraw_plan);

    auto reference = harness.device->create_render_target(target_desc());
    paint(harness, scene, *reference, nullptr);

    const std::vector<std::uint8_t> got = read_target(*harness.device, *incremental);
    const std::vector<std::uint8_t> want = read_target(*harness.device, *reference);
    const auto diff =
        tessera::rendering::golden::compare_rgba({kSize, kSize}, got, {kSize, kSize}, want, 0);
    ok = expect(diff.mismatched == 0,
                "damage-limited redraw matches a full redraw pixel for pixel") &&
         ok;
    if (diff.mismatched != 0) {
        std::cerr << "filter_damage_gpu: mismatched pixels: " << diff.mismatched << "\n";
    }
    // The scene really changed, so equality is not the two frames being blank.
    const std::size_t center = (55u * kSize + 55u) * 4u;
    ok = expect(want[center] > 200 && want[center + 1] < 40,
                "the moved child is drawn (guards against comparing two empty frames)") &&
         ok;
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
        std::cout << "filter_damage_gpu: skipped (Vulkan device unavailable: " << error.what()
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

    bool ok = damaged_redraw_equals_full_redraw(harness);
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "filter_damage_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "filter_damage_gpu: ok\n";
    return 0;
}
