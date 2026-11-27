// SPDX-License-Identifier: MIT
//
// Frame cost of retained layers (issue #617, docs/design/gpu/offscreen-layers-retention.md section
// 7): the same 1920x1080 scene of masked and blurred cards painted every frame through the real
// paint path, three ways (the whole-frame layer box of every layer, the node's own box without a
// cache, and retained), on the null backend (CPU cost of painting and recording, plus the
// pass/draw/copy counts) and, when a device exists, on Vulkan (wall time per frame including the
// GPU: submit() waits). Prints one line per run; not a pass/fail test.
//
//   layer_retain_bench [--frames N] [--cards N]

#include "layer_hazard_device.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;

class StubStyles final : public tessera::ui::paint::StyleProvider {
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

void clear_tree(Node& node) {
    node.clear_dirty(tessera::ui::Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// A grid of cards: three of four masked (gradient mask, opacity 0.9), one of four blurred. Each
// card holds eight children, so a card is a real subtree and not a single box.
struct Scene {
    Node root{1};
    StubStyles styles;
    std::uint32_t cards = 0;

    explicit Scene(std::uint32_t count)
        : cards(count) {
        root.set_bounds({0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)});
        ComputedStyle page;
        page.set("background-color", "#101830");
        styles.set(1, page);
        NodeId next = 2;
        const std::uint32_t columns = 4;
        for (std::uint32_t card = 0; card < count; ++card) {
            auto node = std::make_unique<Node>(next);
            const float x = 20.0f + static_cast<float>(card % columns) * 470.0f;
            const float y = 20.0f + static_cast<float>(card / columns) * 262.0f;
            node->set_bounds({x, y, 440.0f, 230.0f});
            ComputedStyle style;
            style.set("background-color", "#2850a0");
            style.set("border-radius", "16px");
            if (card % 4 == 3) {
                style.set("filter", "blur(6px)");
            } else {
                style.set("mask-image", "linear-gradient(90deg, rgba(0,0,0,0.2), #000)");
                style.set("opacity", "0.9");
            }
            styles.set(next, style);
            ++next;
            for (int child = 0; child < 8; ++child) {
                auto leaf = std::make_unique<Node>(next);
                leaf->set_bounds({12.0f + static_cast<float>(child % 4) * 104.0f,
                                  16.0f + static_cast<float>(child / 4) * 100.0f, 96.0f, 88.0f});
                ComputedStyle leaf_style;
                leaf_style.set("background-color", child % 2 == 0 ? "#f0c040" : "#40c0a0");
                leaf_style.set("border-radius", "10px");
                styles.set(next, leaf_style);
                ++next;
                node->add_child(std::move(leaf));
            }
            root.add_child(std::move(node));
        }
        clear_tree(root);
    }
};

struct Result {
    double mean_ms = 0.0;
    double median_ms = 0.0;
    double worst_ms = 0.0;
    std::uint32_t passes = 0;
    std::uint32_t draws = 0;
    std::uint32_t copies = 0;
    std::uint64_t hits = 0;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kWidth, kHeight};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

template<typename Stats>
Result run(rhi::Device& device, rhi::RenderTarget& target, rhi::Pipeline& pipeline,
           rhi::Texture& white, GlassPipelines glass, bool retain, std::uint32_t retained_layers,
           std::uint32_t cards, int frames, Stats stats_of) {
    Scene scene(cards);
    tessera::ui::paint::PaintCache cache;
    Batched2DConfig config;
    config.frames_in_flight = 1;
    config.max_retained_layers = retained_layers;
    Batched2DRenderer renderer(device, pipeline, white, config, glass);
    std::vector<double> times;
    Result result;
    for (int frame = 0; frame < frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        auto commands = device.create_command_list();
        renderer.begin(*commands, target, target_desc());
        tessera::ui::paint::Batched2DSink sink(renderer);
        tessera::ui::paint::PaintContext context;
        context.paint_cache = &cache;
        context.retain_layers = retain;
        (void)tessera::ui::paint::paint_scene(scene.root, scene.styles, sink, context);
        renderer.end();
        commands->submit();
        const auto stop = std::chrono::steady_clock::now();
        if (frame >= 3) { // the first frames record; the steady state is what a static scene pays
            times.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        }
        stats_of(*commands, result);
    }
    std::sort(times.begin(), times.end());
    double total = 0.0;
    for (const double t : times) {
        total += t;
    }
    result.mean_ms = total / static_cast<double>(times.size());
    result.median_ms = times[times.size() / 2];
    result.worst_ms = times.back();
    result.hits = renderer.retained_layer_stats().hits;
    return result;
}

void print(const char* backend, const char* mode, const Result& r, bool counts) {
    std::cout << std::fixed << std::setprecision(3) << backend << " " << mode << ": mean "
              << r.mean_ms << " ms, median " << r.median_ms << " ms, worst " << r.worst_ms << " ms";
    if (counts) {
        std::cout << ", passes " << r.passes << ", draws " << r.draws << ", copies " << r.copies;
    }
    std::cout << ", cache hits " << r.hits << "\n";
}

} // namespace

int main(int argc, char** argv) {
    int frames = 303;
    std::uint32_t cards = 16;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--frames") {
            frames = std::atoi(argv[i + 1]) + 3;
        } else if (std::string(argv[i]) == "--cards") {
            cards = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        }
    }
    std::cout << cards << " cards (" << cards - cards / 4 << " masked, " << cards / 4
              << " blurred), 8 children each, " << kWidth << "x" << kHeight << ", " << frames - 3
              << " steady frames\n";

    {
        layer_hazard::Log log;
        layer_hazard::Device device(log);
        auto target = device.create_render_target(target_desc());
        auto white = device.create_texture(rhi::TextureDesc{{1, 1}});
        auto pipeline = device.create_pipeline(rhi::PipelineDesc{});
        auto blur = device.create_pipeline(rhi::PipelineDesc{});
        auto composite = device.create_pipeline(rhi::PipelineDesc{});
        rhi::PipelineDesc mask_desc;
        mask_desc.blend = rhi::BlendMode::DestinationIn;
        auto mask = device.create_pipeline(mask_desc);
        const GlassPipelines glass{blur.get(), composite.get(), mask.get()};
        const auto stats = [](rhi::CommandList& commands, Result& result) {
            const auto& s = static_cast<layer_hazard::CommandList&>(commands).stats();
            result.passes = s.begin_render_passes;
            result.draws = s.draws;
            result.copies = s.region_copies;
        };
        // uncached: the whole-frame layer box of every layer; tight: the node's own box, no cache
        // (the entry cap is 0); retained: the node's own box, kept between frames.
        for (const auto& [name, retain, entries] :
             {std::tuple{"uncached ", false, 32u}, std::tuple{"tight box", true, 0u},
              std::tuple{"retained ", true, 32u}}) {
            print("null  ", name,
                  run(device, *target, *pipeline, *white, glass, retain, entries, cards, frames,
                      stats),
                  true);
        }
    }

    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    std::unique_ptr<rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan: skipped (" << error.what() << ")\n";
        return 0;
    }
    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    auto pipeline =
        device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc(format));
    auto blur = device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    auto composite = device->create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
    auto mask = device->create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc(
        format, rhi::BlendMode::DestinationIn));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    auto target = device->create_render_target(target_desc());
    const GlassPipelines glass{blur.get(), composite.get(), mask.get()};
    const auto no_stats = [](rhi::CommandList&, Result&) {};
    for (const auto& [name, retain, entries] :
         {std::tuple{"uncached ", false, 32u}, std::tuple{"tight box", true, 0u},
          std::tuple{"retained ", true, 32u}}) {
        print("vulkan", name,
              run(*device, *target, *pipeline, *white, glass, retain, entries, cards, frames,
                  no_stats),
              false);
    }
    return 0;
}
