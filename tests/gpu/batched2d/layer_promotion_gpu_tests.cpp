// SPDX-License-Identifier: MIT
//
// Generic layer promotion (issue #1014, rest of #967; docs/design/gpu/offscreen-layers-retention.md
// section 11) on a real device: a frame painted with a promoted (retained) container must match the
// same frame painted flat. Promotion is opt-in until this is proven.
//
// One scene, two renderers on one device, the same frame sequence painted through the real paint
// path (paint_scene -> Batched2DSink -> renderer): clean frames until the container is promoted,
// recorded and composited from the cache, a content change (the stale entry must be released at
// once), and clean frames that promote again. The control paints flat with promotion off. Two
// renderer settings run: AA off (the golden rasterizer setting, byte-identical required) and AA on,
// where the composite may differ by one rounding step at anti-aliased edges (at most 1 per
// channel).
//
// Skips cleanly when the machine has no Vulkan device; validation-clean when it has one.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/layer_promotion.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kSize = 128;
constexpr int kChildren = 24;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_promotion_gpu: " << message << "\n";
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
    std::unique_ptr<rhi::Pipeline> mask_out;
    std::unique_ptr<rhi::Pipeline> mask_xor;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
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

// root(1); container(2) at (8,8) 112x112 with kChildren rounded children (ids 10..) so it emits
// enough draws to qualify, and rounded corners to leave anti-aliased edges inside the layer.
struct Scene {
    Node root{1};
    StubStyles styles;

    Scene() {
        root.set_bounds({0.0f, 0.0f, kSize, kSize});
        auto container = std::make_unique<Node>(2);
        container->set_bounds({8.0f, 8.0f, 112.0f, 112.0f});
        for (int index = 0; index < kChildren; ++index) {
            auto child = std::make_unique<Node>(static_cast<NodeId>(10 + index));
            child->set_bounds({4.0f + static_cast<float>(index % 6) * 17.5f,
                               4.0f + static_cast<float>(index / 6) * 25.5f, 15.5f, 22.25f});
            ComputedStyle style;
            style.set("background-color", index % 2 == 0 ? "#f0c040" : "#40c0a0");
            style.set("border-radius", "5px");
            styles.set(child->id(), style);
            container->add_child(std::move(child));
        }
        root.add_child(std::move(container));
        ComputedStyle card;
        card.set("background-color", "#2850a0");
        card.set("border-radius", "9px");
        styles.set(2, card);
        clear_tree(root);
    }
    Node& child() {
        return *root.children()[0]->children()[3];
    }
    void recolor(const char* color) {
        ComputedStyle style;
        style.set("background-color", color);
        style.set("border-radius", "5px");
        styles.set(child().id(), style);
    }
};

struct Run {
    std::vector<std::vector<std::uint8_t>> frames;
    std::uint64_t hits = 0;
    std::uint64_t records = 0;
    std::uint64_t invalidated = 0;
    std::uint32_t entries_after_change = 99;
    std::uint64_t promoted_frames = 0;
};

constexpr int kFrames = 12;
constexpr int kChangeAt = 6;

Run run_sequence(Harness& harness, bool promote, bool aa) {
    Scene scene;
    tessera::ui::paint::PaintCache cache;
    tessera::ui::paint::LayerPromotionConfig policy_config;
    policy_config.min_draws = 8;
    policy_config.min_area = 1000.0f;
    policy_config.min_stable_frames = 2;
    tessera::ui::paint::LayerPromotion policy(policy_config);
    Batched2DConfig config;
    config.enable_aa = aa;
    config.golden_mode = !aa;
    Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                               GlassPipelines{harness.blur.get(), harness.composite.get(),
                                              harness.mask.get(), harness.mask_out.get(),
                                              harness.mask_xor.get()});
    auto target = harness.device->create_render_target(target_desc());
    Run run;
    for (int frame = 0; frame < kFrames; ++frame) {
        if (frame == kChangeAt) {
            scene.recolor("#ff3232");
            scene.child().mark_dirty(Dirty::Paint);
        } else if (frame == kChangeAt + 1) {
            clear_tree(scene.root);
        }
        auto commands = harness.device->create_command_list();
        renderer.begin(*commands, *target, target_desc());
        tessera::ui::paint::Batched2DSink sink(renderer);
        tessera::ui::paint::PaintContext context;
        context.paint_cache = &cache;
        context.retain_layers = promote;
        context.layer_promotion = promote ? &policy : nullptr;
        const auto stats = tessera::ui::paint::paint_scene(scene.root, scene.styles, sink, context);
        run.promoted_frames += stats.promoted_layers;
        renderer.end();
        commands->submit();
        run.frames.push_back(read_target(*harness.device, *target));
        if (frame == kChangeAt) {
            run.entries_after_change = renderer.retained_layer_stats().entries;
        }
    }
    run.hits = renderer.retained_layer_stats().hits;
    run.records = renderer.retained_layer_stats().records;
    run.invalidated = renderer.retained_layer_stats().invalidated;
    return run;
}

// Largest per-channel difference between two frames, and how many bytes differ.
std::pair<int, std::size_t> compare(const std::vector<std::uint8_t>& a,
                                    const std::vector<std::uint8_t>& b) {
    int worst = 0;
    std::size_t differing = 0;
    for (std::size_t index = 0; index < a.size(); ++index) {
        const int delta = std::abs(static_cast<int>(a[index]) - static_cast<int>(b[index]));
        worst = std::max(worst, delta);
        differing += delta != 0 ? 1u : 0u;
    }
    return {worst, differing};
}

bool same_pixels(Harness& harness, bool aa) {
    const char* name = aa ? "AA on" : "AA off";
    const Run promoted = run_sequence(harness, true, aa);
    const Run flat = run_sequence(harness, false, aa);
    bool ok = expect(promoted.frames.size() == flat.frames.size(), name);
    int worst_overall = 0;
    for (std::size_t frame = 0; ok && frame < promoted.frames.size(); ++frame) {
        const auto [worst, differing] = compare(promoted.frames[frame], flat.frames[frame]);
        worst_overall = std::max(worst_overall, worst);
        if (differing != 0) {
            std::cerr << "layer_promotion_gpu: " << name << ": frame " << frame << " differs in "
                      << differing << " bytes, worst channel delta " << worst << "\n";
        }
        // AA off must be byte-for-byte; AA on may round one step differently at anti-aliased edges.
        ok = expect(aa ? worst <= 1 : differing == 0,
                    aa ? "a promoted frame is within one rounding step of the flat frame"
                       : "a promoted frame is byte-identical to the flat frame") &&
             ok;
    }
    std::cout << "layer_promotion_gpu: " << name << ": worst channel delta " << worst_overall
              << "\n";
    ok = expect(promoted.records >= 2 && promoted.hits >= 2 && promoted.promoted_frames >= 4,
                "the promoted run really recorded and composited a layer") &&
         ok;
    ok = expect(promoted.entries_after_change == 0 && promoted.invalidated >= 1,
                "the stale entry is released the frame the promoted node turns dirty") &&
         ok;
    ok = expect(flat.records == 0 && flat.hits == 0, "the flat control never retains") && ok;
    ok = expect(promoted.frames[kChangeAt] != promoted.frames[kChangeAt - 1],
                "the content change is visible, so the comparison is not vacuous") &&
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
        std::cout << "layer_promotion_gpu: skipped (Vulkan device unavailable: " << error.what()
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
    harness.mask_out = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::SourceOut));
    harness.mask_xor = harness.device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Xor));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = same_pixels(harness, false);
    ok = same_pixels(harness, true) && ok;
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_promotion_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_promotion_gpu: ok\n";
    return 0;
}
