// SPDX-License-Identifier: MIT
//
// The per-fragment gradients against Chromium (ADR-076, issue #617): fifteen scenes - linear,
// radial, conic, repeating and translucent gradients, as `background-image` and as `mask-image` -
// plus twelve colour-stop-position scenes (two positions, px lengths, positions outside the line,
// conic angles; gradient_chromium_reference_stops.hpp), painted through the real paint path and
// Batched2DSink on a real device, compared sample by sample with what Chromium drew
// (gradient_chromium_reference.hpp, 69 samples per scene).
//
// Every scene is rendered twice, once with the shader and once with the banded fallback
// (`Batched2DConfig::analytic_gradients` false), so the error the shader removed is a measured pair
// of numbers printed per scene, and the test requires the shader never to be further from Chromium
// than the bands were. A sample that sits on a hard edge in Chromium's own series (a step of more
// than 120 against a neighbour two pixels away: a conic seam, a repeat wrap) is not compared:
// whether the edge pixel falls on one side or the other is the rasteriser's rule, not the ramp's.
//
// Skips cleanly when the machine has no Vulkan device.

#include "gradient_chromium_reference.hpp"
#include "gradient_chromium_reference_stops.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
namespace reference = gradient_chromium_reference;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;

constexpr std::uint32_t kSize = 64;

// The most the shader may be from Chromium on a compared sample, out of 255, per scene. The bands'
// own errors are printed beside it. Measured 2026-11-27 on the dev PC (AMD) and on lavapipe.
struct Budget {
    const char* name;
    int shader;
};
constexpr std::array<Budget, 27> kBudgets{{
    {"mask-linear@1", 2},
    {"mask-linear@0.5", 2},
    {"mask-radial@1", 2},
    {"mask-radial@0.5", 2},
    {"mask-conic@1", 2},
    {"mask-conic@0.5", 2},
    {"linear-63deg-3stop", 2},
    {"radial-closest-side", 2},
    {"radial-farthest-corner", 2},
    {"conic-hard-from30", 2},
    {"conic-smooth-from200", 2},
    {"repeating-linear", 2},
    {"repeating-radial", 2},
    {"translucent-same-hue", 2},
    {"translucent-mixed-hue", 2},
    // Colour-stop positions (gradient_chromium_reference_stops.hpp): two positions, px lengths,
    // positions outside the line, conic angles, a degenerate repeat.
    {"repeating-linear-px", 2},
    {"repeating-linear-open", 2},
    {"two-positions", 2},
    {"stripes-px-pairs", 2},
    {"repeating-radial-px", 2},
    {"repeating-ellipse-px", 2},
    {"repeating-conic-angles", 2},
    {"conic-two-angles", 2},
    {"linear-beyond-line", 2},
    {"mixed-units", 2},
    {"short-period", 2},
    {"degenerate-repeat", 2},
}};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "gradient_chromium: " << message << "\n";
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

// One scene through the paint path. `analytic` false forces the banded fallback.
std::vector<std::uint8_t> render(Harness& harness, const reference::Scene& scene, bool analytic,
                                 std::uint32_t& quads) {
    using tessera::ui::Node;
    using tessera::ui::style::ComputedStyle;
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 64.0f, 64.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds({scene.box[0], scene.box[1], scene.box[2], scene.box[3]});
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    if (scene.mask) {
        style.set("background-color", "#00ff00");
        style.set("opacity", std::to_string(scene.opacity));
        style.set("mask-image", scene.css);
    } else {
        style.set("background-image", scene.css);
    }
    styles.set(2, style);
    clear_tree(root);

    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    config.analytic_gradients = analytic;
    {
        Batched2DRenderer renderer(
            *harness.device, *harness.pipeline, *harness.white, config,
            GlassPipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()});
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        quads = renderer.gradient_quads();
    }
    return read_target(*harness.device, *target);
}

struct Comparison {
    int worst = 0;
    int compared = 0;
    int skipped_edges = 0;
    std::string where;
};

int channel_of(std::uint32_t packed, int channel) {
    return static_cast<int>((packed >> (16 - 8 * channel)) & 0xFFU);
}

// A sample on a hard edge of Chromium's own series: a big step against a neighbour two pixels away.
bool is_edge(const std::array<std::uint32_t, reference::kSamples>& series, int index) {
    for (const int neighbour : {index - 1, index + 1}) {
        if (neighbour < 0 || neighbour >= reference::kSamples) {
            continue;
        }
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(channel_of(series[index], channel) -
                         channel_of(series[neighbour], channel)) > 120) {
                return true;
            }
        }
    }
    return false;
}

void compare_series(const std::vector<std::uint8_t>& pixels,
                    const std::array<std::uint32_t, reference::kSamples>& series, const char* line,
                    int (*x_of)(int), int (*y_of)(int), Comparison& result) {
    for (int index = 0; index < reference::kSamples; ++index) {
        if (is_edge(series, index)) {
            ++result.skipped_edges;
            continue;
        }
        const int at = 10 + 2 * index;
        const std::size_t offset =
            (static_cast<std::size_t>(y_of(at)) * kSize + static_cast<std::size_t>(x_of(at))) * 4;
        for (int channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(static_cast<int>(pixels[offset + channel]) -
                                       channel_of(series[index], channel));
            ++result.compared;
            if (delta > result.worst) {
                result.worst = delta;
                result.where = std::string(line) + " at " + std::to_string(at) + " channel " +
                               std::to_string(channel);
            }
        }
    }
}

Comparison compare(const std::vector<std::uint8_t>& pixels, const reference::Scene& scene) {
    Comparison result;
    compare_series(
        pixels, scene.row32, "row 32", [](int at) { return at; }, [](int) { return 32; }, result);
    compare_series(
        pixels, scene.col32, "column 32", [](int) { return 32; }, [](int at) { return at; },
        result);
    compare_series(
        pixels, scene.diagonal, "diagonal", [](int at) { return at; }, [](int at) { return at; },
        result);
    return result;
}

int budget_for(const char* name) {
    for (const Budget& budget : kBudgets) {
        if (std::string(budget.name) == name) {
            return budget.shader;
        }
    }
    return -1;
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
        std::cout << "gradient_chromium: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }
    if (!harness.device->supports_analytic_gradients()) {
        std::cout << "gradient_chromium: skipped (no analytic gradients on this device)\n";
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

    bool ok = true;
    int worst_shader = 0;
    int worst_banded = 0;
    std::vector<const reference::Scene*> scenes;
    for (const reference::Scene& scene : reference::kScenes) {
        scenes.push_back(&scene);
    }
    for (const reference::Scene& scene : reference::kStopScenes) {
        scenes.push_back(&scene);
    }
    for (const reference::Scene* scene_ptr : scenes) {
        const reference::Scene& scene = *scene_ptr;
        std::uint32_t shader_quads = 0;
        std::uint32_t banded_quads = 0;
        const Comparison shader = compare(render(harness, scene, true, shader_quads), scene);
        const Comparison banded = compare(render(harness, scene, false, banded_quads), scene);
        std::cout << "gradient_chromium: " << scene.name << ": bands " << banded.worst
                  << "/255, shader " << shader.worst << "/255 (" << shader.compared
                  << " channel samples, " << shader.skipped_edges << " edge samples skipped)\n";
        worst_shader = std::max(worst_shader, shader.worst);
        worst_banded = std::max(worst_banded, banded.worst);
        ok =
            expect(shader_quads > 0 && banded_quads == 0,
                   std::string(scene.name) + ": the shader path and the fallback are each taken") &&
            ok;
        const int budget = budget_for(scene.name);
        ok = expect(budget >= 0, std::string(scene.name) + ": has a budget") && ok;
        if (shader.worst > budget) {
            std::cerr << "gradient_chromium: " << scene.name << " is " << shader.worst
                      << "/255 from Chromium at " << shader.where << " (budget " << budget << ")\n";
            ok = false;
        }
        if (shader.worst > banded.worst) {
            std::cerr << "gradient_chromium: " << scene.name
                      << ": the shader is further from Chromium than the bands were\n";
            ok = false;
        }
        if (shader.compared < 50) {
            std::cerr << "gradient_chromium: " << scene.name << ": too few samples compared\n";
            ok = false;
        }
    }
    std::cout << "gradient_chromium: worst over all scenes: bands " << worst_banded
              << "/255, shader " << worst_shader << "/255\n";

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "gradient_chromium: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "gradient_chromium: ok\n";
    return 0;
}
