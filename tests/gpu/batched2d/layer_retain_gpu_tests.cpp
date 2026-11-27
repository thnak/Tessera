// SPDX-License-Identifier: MIT
//
// Retained layers (issue #617, docs/design/gpu/offscreen-layers.md section 5.6) on a real device:
// a retained frame must be pixel-identical to the same frame painted without retention.
//
// Each scene is painted through the real paint path (paint_scene -> Batched2DSink -> renderer) as
// one sequence of frames on one renderer and target, twice: once with retention and once without it
// (the uncached control). The sequence is: a full frame that records the layers, a full frame that
// composites them, two damage-clipped frames (a damage box that cuts the node in part, then
// another), a full frame, a content change (a descendant restyled and marked dirty), and two clean
// frames that record again and hit. Every frame's pixels must be byte-identical between the two
// runs, and the retained run must really have used the cache (records, hits, fewer layer passes).
// The scenes cover a gradient mask under opacity with overlapping children, a blur, a drop-shadow
// with a mask, and an isolated opacity group.
//
// Skips cleanly when the machine has no Vulkan device; validation-clean when it has one.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
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

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::RedrawPlan;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kSize = 96;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_retain_gpu: " << message << "\n";
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

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
    std::unique_ptr<rhi::Pipeline> mask;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc(rhi::LoadOp load) {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = load;
    desc.clear_color = rhi::rgba8(20, 30, 60);
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// What a scene is: the layered node's style, and whether the paint context isolates group opacity.
struct SceneSpec {
    const char* name;
    std::initializer_list<std::pair<const char*, const char*>> style;
    bool isolate_opacity;
    // A filter reads past the damage it is cut to, so the real pipeline never damages part of a
    // filtered box (damage adds the whole box and its halo); such a scene is cut to the whole of
    // it.
    bool whole_box_damage;
};

// root(1); node(2) at (16,16) 56x56 carrying the scene's style; two overlapping children (3, 4).
struct Scene {
    Node root{1};
    StubStyles styles;
    bool isolate_opacity = false;

    explicit Scene(const SceneSpec& spec)
        : isolate_opacity(spec.isolate_opacity) {
        root.set_bounds({0.0f, 0.0f, kSize, kSize});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({16.0f, 16.0f, 56.0f, 56.0f});
        auto first = std::make_unique<Node>(3);
        first->set_bounds({4.0f, 4.0f, 30.0f, 30.0f});
        auto second = std::make_unique<Node>(4);
        second->set_bounds({20.0f, 18.0f, 30.0f, 30.0f});
        node->add_child(std::move(first));
        node->add_child(std::move(second));
        root.add_child(std::move(node));
        ComputedStyle layered;
        layered.set("background-color", "#00c864");
        for (const auto& [property, value] : spec.style) {
            layered.set(property, value);
        }
        styles.set(2, layered);
        recolor("#ff3232");
        ComputedStyle second_style;
        second_style.set("background-color", "#3264ff");
        styles.set(4, second_style);
        clear_tree(root);
    }
    void recolor(const char* color) {
        ComputedStyle style;
        style.set("background-color", color);
        styles.set(3, style);
    }
    Node& child() {
        return *root.children()[0]->children()[0];
    }
};

struct Run {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint64_t hits = 0;
    std::uint64_t records = 0;
    std::uint32_t isolated_last = 0;
};

// One frame. `damage` null: a full frame onto a cleared target; else a damage-clipped frame
// that loads the target as it was.
void paint_frame(Harness& harness, Batched2DRenderer& renderer, rhi::RenderTarget& target,
                 Scene& scene, tessera::ui::paint::PaintCache& cache, bool retain,
                 const rhi::Scissor* damage, std::vector<std::vector<std::uint8_t>>& frames) {
    auto commands = harness.device->create_command_list();
    const rhi::RenderTargetDesc desc =
        target_desc(damage == nullptr ? rhi::LoadOp::Clear : rhi::LoadOp::Load);
    if (damage != nullptr) {
        RedrawPlan plan;
        plan.scissors.push_back(*damage);
        renderer.begin_damaged(*commands, target, desc, plan);
    } else {
        renderer.begin(*commands, target, desc);
    }
    tessera::ui::paint::Batched2DSink sink(renderer);
    tessera::ui::paint::PaintContext context;
    context.paint_cache = &cache;
    context.retain_layers = retain;
    context.isolate_group_opacity = scene.isolate_opacity;
    (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink, context);
    renderer.end();
    commands->submit();
    frames.push_back(read_target(*harness.device, target));
}

Run run_sequence(Harness& harness, const SceneSpec& spec, bool retain) {
    Scene scene(spec);
    tessera::ui::paint::PaintCache cache;
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Batched2DRenderer renderer(
        *harness.device, *harness.pipeline, *harness.white, config,
        GlassPipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()});
    auto target = harness.device->create_render_target(target_desc(rhi::LoadOp::Clear));
    Run run;
    const rhi::Scissor cut_node =
        spec.whole_box_damage ? rhi::Scissor{4, 4, 80, 80} : rhi::Scissor{10, 10, 30, 30};
    const rhi::Scissor cut_edge =
        spec.whole_box_damage ? rhi::Scissor{8, 6, 88, 88} : rhi::Scissor{50, 20, 40, 60};
    const auto full = [&] {
        paint_frame(harness, renderer, *target, scene, cache, retain, nullptr, run.frames);
    };
    const auto damaged = [&](const rhi::Scissor& region) {
        paint_frame(harness, renderer, *target, scene, cache, retain, &region, run.frames);
    };
    full();            // records
    full();            // hits
    damaged(cut_node); // hits, composite cut to the damage
    damaged(cut_edge);
    full();
    // A descendant changes: the layer must render again, and show the change.
    scene.recolor("#ffe632");
    scene.child().mark_dirty(Dirty::Paint);
    full();
    clear_tree(scene.root);
    full();            // records again
    damaged(cut_node); // hits
    full();
    run.hits = renderer.retained_layer_stats().hits;
    run.records = renderer.retained_layer_stats().records;
    run.isolated_last = renderer.layers_isolated();
    return run;
}

bool same_pixels(Harness& harness, const SceneSpec& spec) {
    const Run cached = run_sequence(harness, spec, true);
    const Run control = run_sequence(harness, spec, false);
    bool ok = expect(cached.frames.size() == control.frames.size(), spec.name);
    for (std::size_t frame = 0; ok && frame < cached.frames.size(); ++frame) {
        if (cached.frames[frame] != control.frames[frame]) {
            std::size_t first = 0;
            while (cached.frames[frame][first] == control.frames[frame][first]) {
                ++first;
            }
            std::cerr << "layer_retain_gpu: " << spec.name << ": frame " << frame
                      << " differs from the uncached paint at byte " << first << " ("
                      << static_cast<int>(cached.frames[frame][first]) << " vs "
                      << static_cast<int>(control.frames[frame][first]) << ")\n";
            ok = false;
        }
    }
    ok = expect(cached.records == 2 && cached.hits == 6,
                "the layer was recorded twice and composited from the cache six times") &&
         ok;
    ok = expect(control.records == 0 && control.hits == 0, "the control never retains") && ok;
    ok = expect(cached.isolated_last == 0 && control.isolated_last == 1,
                "the last clean frame drew no layer when retained, one when not") &&
         ok;
    // The frames must differ from one another where the scene changed, or the comparison is
    // vacuous.
    ok = expect(cached.frames[4] != cached.frames[5], "the content change is visible") && ok;
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
        std::cout << "layer_retain_gpu: skipped (Vulkan device unavailable: " << error.what()
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

    const SceneSpec scenes[] = {
        {"mask + opacity",
         {{"mask-image", "linear-gradient(90deg, rgba(0,0,0,0.1), #000)"}, {"opacity", "0.8"}},
         false,
         false},
        {"blur", {{"filter", "blur(2px)"}}, false, true},
        {"drop-shadow + mask",
         {{"filter", "drop-shadow(#000000 3px 3px 2px)"},
          {"mask-image", "radial-gradient(circle, #000, rgba(0,0,0,0.3))"}},
         false,
         true},
        {"isolated opacity group", {{"opacity", "0.5"}}, true, false},
    };
    bool ok = true;
    for (const SceneSpec& spec : scenes) {
        ok = same_pixels(harness, spec) && ok;
    }
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_retain_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_retain_gpu: ok\n";
    return 0;
}
