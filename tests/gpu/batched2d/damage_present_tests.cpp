// SPDX-License-Identifier: MIT
//
// Render-side damage reduction in the present loop (D4): a scripted small-change sequence is
// rendered cumulatively through `Batched2DRenderer::begin_damaged` into one offscreen target and
// compared byte-for-byte with a full redraw through the same renderer/sink (same glyph-atlas
// history; a fresh renderer can pack the atlas differently and change filtered glyph edges). Small
// changes must use a partial plan (few regions, less area), a whole-screen change must fall back to
// full redraw, and the script must be byte-identical across two runs. Empty damage must not render.
// Skips cleanly without a Vulkan device or the fixture font; fails on any validation message.

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/redraw_plan.hpp"
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

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::should_full_redraw;
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
using tessera::ui::PaintData;
using tessera::ui::PaintWrap;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr Extent2D kExtent{240, 160};
constexpr float kTargetArea = static_cast<float>(kExtent.width) * kExtent.height;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_present: " << message << "\n";
    }
    return condition;
}

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

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

struct Scene {
    Node root{1};
    Node* card = nullptr;
    Node* text = nullptr;
    Node* accent = nullptr;
    MapStyles styles;

    Scene() {
        root.set_bounds(
            {0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
        card = &root.add_child(std::make_unique<Node>(2));
        card->set_bounds({20, 20, 60, 40});
        text = &root.add_child(std::make_unique<Node>(3));
        text->set_bounds({20, 80, 160, 56});
        accent = &root.add_child(std::make_unique<Node>(4));
        accent->set_bounds({190, 20, 30, 30});

        styles.set(1, style({{"background-color", "#0B1220"}}));
        styles.set(2, style({{"background-color", "#EF4444"}}));
        styles.set(3, style({{"background-color", "#0F172A"},
                             {"color", "#E2E8F0"},
                             {"font-size", "14px"},
                             {"padding-left", "4px"},
                             {"padding-right", "4px"}}));
        styles.set(4, style({{"background-color", "#38BDF8"}}));

        PaintData payload;
        payload.text = "Damage redraw keeps the previous frame";
        payload.wrap = PaintWrap::Wrap;
        payload.text_style.font_size = 14.0f;
        payload.text_style.color = rgba8(226, 232, 240);
        text->set_paint_data(std::move(payload));
    }

    void change_text(std::string value) {
        PaintData payload;
        payload.text = std::move(value);
        payload.wrap = PaintWrap::Wrap;
        payload.text_style.font_size = 14.0f;
        payload.text_style.color = rgba8(226, 232, 240);
        text->set_paint_data(std::move(payload));
    }

    void toggle_accent() {
        styles.set(4, style({{"background-color", "#F97316"}}));
    }
};

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

std::uint64_t covered_area(const RedrawPlan& plan) {
    std::uint64_t area = 0;
    for (const auto& scissor : plan.scissors) {
        area += static_cast<std::uint64_t>(scissor.width) * scissor.height;
    }
    return area;
}

struct RunResult {
    std::vector<std::uint8_t> final_pixels;
    std::vector<std::uint8_t> reference_pixels;
    std::vector<bool> full_redraw;
    std::vector<std::uint64_t> areas;
    std::vector<std::size_t> regions;
    std::vector<std::uint32_t> draw_calls;
    std::uint32_t idle_draws = 0;
};

RunResult run_script(Device& device, const tessera::ui::text::FontFace& face) {
    RunResult result;
    Scene scene;

    auto pipeline = device.create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device.create_texture(white_desc);
    tessera::ui::text::GlyphRasterizer rasterizer(face);
    tessera::ui::text::HarfBuzzShaper shaper(face);
    tessera::ui::text::TextRenderer text_renderer(device, rasterizer, {256, 256, 2, 1});
    Batched2DRenderer renderer(device, *pipeline, *white, Batched2DConfig{4096, false, true});
    Batched2DSink sink(renderer, &text_renderer, &shaper);

    auto target = device.create_render_target(target_desc(LoadOp::Clear));
    DamageAccumulator damage(DamageConfig{kExtent, 8});

    const auto plan_for = [&]() {
        RedrawPlan plan = make_redraw_plan(damage);
        if (should_full_redraw(plan, kExtent) ||
            static_cast<float>(covered_area(plan)) >= 0.6f * kTargetArea) {
            plan.full_redraw = true;
        }
        return plan;
    };

    const auto frame = [&](const RedrawPlan& plan, bool clear) {
        auto commands = device.create_command_list();
        RenderTargetDesc desc = target_desc(clear ? LoadOp::Clear : LoadOp::Load);
        if (clear || plan.full_redraw) {
            desc.load_op = LoadOp::Clear;
            renderer.begin(*commands, *target, desc);
        } else {
            renderer.begin_damaged(*commands, *target, desc, plan);
        }
        sink.begin_text_frame();
        paint_scene(scene.root, scene.styles, sink);
        renderer.end();
        commands->submit();
        result.full_redraw.push_back(clear || plan.full_redraw);
        result.areas.push_back(clear ? static_cast<std::uint64_t>(kTargetArea)
                                     : covered_area(plan));
        result.regions.push_back(plan.scissors.size());
        result.draw_calls.push_back(renderer.draw_calls());
    };

    damage.add({0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
    frame(plan_for(), true); // initial frame: full redraw

    damage.clear();
    damage.add({18, 18, 72, 64}); // moved card: old + new bounds
    frame(plan_for(), false);

    damage.clear();
    damage.add({18, 78, 164, 60}); // changed text run
    frame(plan_for(), false);

    damage.clear();
    damage.add({188, 18, 34, 34}); // toggled background
    frame(plan_for(), false);

    damage.clear();
    damage.add({0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
    scene.card->set_bounds(
        {0, 0, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height)});
    scene.change_text("A changed text run should damage only its box");
    scene.toggle_accent();
    frame(plan_for(), false); // whole-screen change: policy falls back to full

    // Full-redraw reference through the same renderer/sink (same glyph atlas history).
    auto reference_target = device.create_render_target(target_desc(LoadOp::Clear));
    auto reference_commands = device.create_command_list();
    const RenderTargetDesc reference_desc = target_desc(LoadOp::Clear);
    renderer.begin(*reference_commands, *reference_target, reference_desc);
    sink.begin_text_frame();
    paint_scene(scene.root, scene.styles, sink);
    renderer.end();
    reference_commands->submit();
    result.reference_pixels = read_target(device, *reference_target);
    result.final_pixels = read_target(device, *target);

    // Empty damage is an idle frame: no render pass and no draw work. A fresh accumulator avoids
    // depending on any latched state (DamageAccumulator::clear() is covered by task-62 tests).
    DamageAccumulator idle_damage(DamageConfig{kExtent, 8});
    const RedrawPlan idle_plan = make_redraw_plan(idle_damage);
    result.idle_draws = 0;
    if (!idle_plan.scissors.empty() || idle_plan.full_redraw) {
        ++result.idle_draws;
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "damage_present: skipped (missing font fixture path)\n";
        return 0;
    }
    const auto loaded = tessera::ui::text::FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cout << "damage_present: skipped (cannot load fixture: " << loaded.error().message
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
        std::cout << "damage_present: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    const RunResult first = run_script(*device, *loaded);
    const RunResult second = run_script(*device, *loaded);

    bool ok = expect(first.final_pixels == first.reference_pixels,
                     "cumulative damaged frames match the same-sink full redraw (tolerance 0)");
    ok = expect(first.final_pixels == second.final_pixels,
                "the script is byte-identical across two runs") &&
         ok;
    ok = expect(first.full_redraw == second.full_redraw && first.areas == second.areas &&
                    first.regions == second.regions,
                "the damage plan sequence is deterministic") &&
         ok;
    ok = expect(first.full_redraw.size() == 5, "the script runs five frames") && ok;
    ok = expect(first.full_redraw[0], "the initial frame is a full redraw") && ok;
    ok = expect(!first.full_redraw[1] && !first.full_redraw[2] && !first.full_redraw[3],
                "small changes use partial damage plans") &&
         ok;
    ok = expect(first.regions[1] > 0 && first.regions[1] <= 2,
                "a moved node damages a small number of regions") &&
         ok;
    ok = expect(static_cast<float>(first.areas[1]) < kTargetArea * 0.5f &&
                    static_cast<float>(first.areas[2]) < kTargetArea * 0.5f &&
                    static_cast<float>(first.areas[3]) < kTargetArea * 0.5f,
                "small changes cover less than half the target") &&
         ok;
    ok = expect(first.full_redraw[4], "a whole-screen change falls back to full redraw") && ok;
    ok = expect(first.idle_draws == 0, "empty damage is an idle frame with no render") && ok;

    if (!ok) {
        std::cerr << "damage_present: failed\n";
        return 1;
    }
    std::cout << "damage_present: ok frames=" << first.full_redraw.size()
              << " partial_regions=" << first.regions[1] << "," << first.regions[2] << ","
              << first.regions[3] << " partial_area=" << first.areas[1] << "," << first.areas[2]
              << "," << first.areas[3] << " draws=" << first.draw_calls[1] << ","
              << first.draw_calls[2] << "," << first.draw_calls[3]
              << " full_draws=" << first.draw_calls[4] << "\n";
    return 0;
}
