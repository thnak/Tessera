// SPDX-License-Identifier: MIT
//
// Layer masks (issue #617, docs/design/gpu/offscreen-layers.md section 5.2) on a real device: the
// pixels the mask path must produce.
//
// Evidence:
//  - a solid layer under a mask keeps the layer where the mask is opaque, half of it under a
//    half-transparent mask, and nothing where the mask drew nothing (the mask target is cleared to
//    transparent, so an undrawn mask is a hole);
//  - the mask composes with the group opacity (a 0.5 layer under an opaque mask is 0.5);
//  - pixels outside the layer's box are untouched;
//  - without the mask pipeline begin_layer_mask() is false and the group paints unmasked;
//  - the real paint path: a node whose stylesheet says `mask-image: linear-gradient(...)` fades
//  from
//    transparent to opaque across its box, through Batched2DSink and the renderer;
//  - the run is validation-clean (mask pass, sampled binds, reopened passes).
//
// Skips cleanly when the machine has no Vulkan device.

#include "layer_mask_chromium_reference.hpp"
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
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassPipelines;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 2; // out of 255

constexpr rhi::Color kRed = rhi::rgba8(255, 0, 0);
constexpr rhi::Rect kBox{8.0f, 8.0f, 48.0f, 48.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_mask_gpu: " << message << "\n";
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

std::array<int, 3> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                            std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2]};
}

bool near(const std::array<int, 3>& actual, int r, int g, int b, const char* what, std::uint32_t x,
          std::uint32_t y) {
    if (std::abs(actual[0] - r) <= kTolerance && std::abs(actual[1] - g) <= kTolerance &&
        std::abs(actual[2] - b) <= kTolerance) {
        return true;
    }
    std::cerr << "layer_mask_gpu: " << what << " at (" << x << "," << y << "): got (" << actual[0]
              << "," << actual[1] << "," << actual[2] << "), expected (" << r << "," << g << ","
              << b << ")\n";
    return false;
}

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t applied = 0;
    std::uint32_t unsupported = 0;
};

// A red box layer at `opacity`, masked by: x in [8, 32) opaque, x in [32, 44) half alpha, nothing
// after. `with_mask` selects whether the renderer owns the mask pipeline.
Frame render(Harness& harness, bool with_mask, float opacity) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        const GlassPipelines pipelines{harness.blur.get(), harness.composite.get(),
                                       with_mask ? harness.mask.get() : nullptr};
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   pipelines);
        renderer.begin(*commands, *target, target_desc());
        renderer.push_layer(kBox, opacity);
        renderer.push_rect(kBox, kRed);
        if (renderer.begin_layer_mask()) {
            renderer.push_rect({8.0f, 8.0f, 24.0f, 48.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
            renderer.push_rect({32.0f, 8.0f, 12.0f, 48.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 0.5f});
            renderer.end_layer_mask();
        }
        renderer.pop_layer();
        renderer.end();
        commands->submit();
        frame.applied = renderer.layer_masks_applied();
        frame.unsupported = renderer.layer_masks_unsupported();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

bool masked_pixels(Harness& harness) {
    const Frame frame = render(harness, true, 1.0f);
    bool ok = expect(frame.applied == 1 && frame.unsupported == 0, "the mask was applied");
    ok = near(pixel_at(frame.pixels, 12, 32), 255, 0, 0, "opaque mask keeps the layer", 12, 32) &&
         ok;
    ok =
        near(pixel_at(frame.pixels, 38, 32), 128, 0, 0, "half mask halves the layer", 38, 32) && ok;
    ok = near(pixel_at(frame.pixels, 50, 32), 0, 0, 0, "an undrawn mask is a hole", 50, 32) && ok;
    ok = near(pixel_at(frame.pixels, 2, 2), 0, 0, 0, "outside the box is untouched", 2, 2) && ok;
    return ok;
}

bool mask_composes_with_opacity(Harness& harness) {
    const Frame frame = render(harness, true, 0.5f);
    bool ok = expect(frame.applied == 1, "the mask was applied");
    ok = near(pixel_at(frame.pixels, 12, 32), 128, 0, 0, "opaque mask under a 0.5 layer", 12, 32) &&
         ok;
    ok = near(pixel_at(frame.pixels, 38, 32), 64, 0, 0, "0.5 mask under a 0.5 layer", 38, 32) && ok;
    ok = near(pixel_at(frame.pixels, 50, 32), 0, 0, 0, "the hole stays a hole", 50, 32) && ok;
    return ok;
}

bool without_the_mask_pipeline(Harness& harness) {
    const Frame frame = render(harness, false, 1.0f);
    bool ok = expect(frame.applied == 0 && frame.unsupported == 1, "the mask was refused");
    ok = near(pixel_at(frame.pixels, 50, 32), 255, 0, 0, "unmasked: the layer shows whole", 50,
              32) &&
         ok;
    return ok;
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

// One green node at (8,8) 48x48 under `mask-image: <css>` at `opacity`, painted through the real
// paint path and Batched2DSink, over a black frame. Returns the frame's pixels.
std::vector<std::uint8_t> render_masked(Harness& harness, const char* css, const char* opacity,
                                        std::uint32_t& isolated, std::uint32_t& applied) {
    using tessera::ui::Node;
    using tessera::ui::style::ComputedStyle;
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 64.0f, 64.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds({8.0f, 8.0f, 48.0f, 48.0f});
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    style.set("background-color", "#00ff00");
    style.set("opacity", opacity);
    style.set("mask-image", css);
    styles.set(2, style);
    clear_tree(root);

    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    {
        Batched2DRenderer renderer(
            *harness.device, *harness.pipeline, *harness.white, config,
            GlassPipelines{harness.blur.get(), harness.composite.get(), harness.mask.get()});
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        applied = renderer.layer_masks_applied();
        isolated = renderer.layers_isolated();
    }
    return read_target(*harness.device, *target);
}

bool paint_path(Harness& harness) {
    std::uint32_t isolated = 0;
    std::uint32_t applied = 0;
    const std::vector<std::uint8_t> pixels = render_masked(
        harness, "linear-gradient(90deg, rgba(0, 0, 0, 0), #000000)", "1", isolated, applied);
    bool ok = expect(isolated == 1 && applied == 1, "the paint pass opened one masked layer");
    const auto left = pixel_at(pixels, 10, 32);
    const auto middle = pixel_at(pixels, 32, 32);
    const auto right = pixel_at(pixels, 54, 32);
    // The ramp is banded, so check monotone growth and the ends rather than exact mid values.
    ok = expect(left[1] < 40, "the left edge of the mask is nearly transparent") && ok;
    ok = expect(middle[1] > 90 && middle[1] < 170, "the middle is about half") && ok;
    ok = expect(right[1] > 215, "the right edge is nearly opaque") && ok;
    ok = expect(left[0] == 0 && middle[0] == 0 && right[0] == 0, "only green shows through") && ok;
    ok = near(pixel_at(pixels, 2, 2), 0, 0, 0, "outside the node is untouched", 2, 2) && ok;
    return ok;
}

// The same scenes Chromium measured (layer_mask_chromium_reference.hpp), through the real paint
// path.
//
// The first version of this comparison found that alpha along a translucent gradient came out up to
// +64 of 255 too high (+119 on a radial diagonal) because the sink grew each solid band by half a
// pixel, so neighbouring bands overlapped and composited a translucent stop twice. The bands now
// abut exactly (band_edge in paint_sink.hpp), and what is left is the documented banding: a linear
// ramp is within 2, the 32x32 radial and conic cells within a few steps of their ramp.
bool chromium_comparison(Harness& harness) {
    struct Scene {
        const char* name;
        const char* css;
        const char* opacity;
        int tolerance; // either direction, out of 255; measured worst case plus a little slack
    };
    const Scene scenes[] = {
        {"linear@1", "linear-gradient(90deg, rgba(0,0,0,0), #000)", "1", 3},
        {"linear@0.5", "linear-gradient(90deg, rgba(0,0,0,0), #000)", "0.5", 3},
        {"radial@1", "radial-gradient(circle, #000, rgba(0,0,0,0))", "1", 10},
        {"radial@0.5", "radial-gradient(circle, #000, rgba(0,0,0,0))", "0.5", 6},
        {"conic@1", "conic-gradient(#000, rgba(0,0,0,0))", "1", 24},
        {"conic@0.5", "conic-gradient(#000, rgba(0,0,0,0))", "0.5", 14},
    };
    bool ok = true;
    for (const Scene& scene : scenes) {
        const layer_mask_reference::Series* reference = nullptr;
        for (const auto& candidate : layer_mask_reference::kChromium) {
            if (std::string(candidate.name) == scene.name) {
                reference = &candidate;
            }
        }
        if (!expect(reference != nullptr, "the scene has a Chromium reference")) {
            return false;
        }
        std::uint32_t isolated = 0;
        std::uint32_t applied = 0;
        const auto pixels = render_masked(harness, scene.css, scene.opacity, isolated, applied);
        ok = expect(isolated == 1 && applied == 1, scene.name) && ok;
        for (std::size_t i = 0; i < layer_mask_reference::kSampleAt.size(); ++i) {
            const std::uint32_t at = layer_mask_reference::kSampleAt[i];
            const int row = pixel_at(pixels, at, 32)[1] - reference->row32[i];
            const int col = pixel_at(pixels, 32, at)[1] - reference->col32[i];
            if (std::abs(row) > scene.tolerance || std::abs(col) > scene.tolerance) {
                std::cerr << "layer_mask_gpu: " << scene.name << " at " << at << ": row "
                          << (row >= 0 ? "+" : "") << row << ", column " << (col >= 0 ? "+" : "")
                          << col << " against Chromium (tolerance " << scene.tolerance << ")\n";
                ok = false;
            }
        }
        // Nothing outside the box, red or blue anywhere in it.
        ok = near(pixel_at(pixels, 4, 4), 0, 0, 0, "outside the box is black", 4, 4) && ok;
        ok = expect(pixel_at(pixels, 32, 32)[0] == 0 && pixel_at(pixels, 32, 32)[2] == 0,
                    "only green shows through") &&
             ok;
    }
    return ok;
}

// The overlap bug itself, without a mask: a translucent `background-image` ramp at a fractional
// position and size, with anti-aliasing on and off. Every interior pixel must be the ideal CSS ramp
// within the band quantisation; a doubly composited band would show as a 20+ step excess.
bool translucent_gradient_is_composited_once(Harness& harness) {
    bool ok = true;
    for (const bool aa : {false, true}) {
        using tessera::ui::Node;
        using tessera::ui::style::ComputedStyle;
        Node root(1);
        root.set_bounds({0.0f, 0.0f, 64.0f, 64.0f});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({8.3f, 8.0f, 47.7f, 48.0f});
        root.add_child(std::move(node));
        StubStyles styles;
        ComputedStyle style;
        style.set("background-image", "linear-gradient(90deg, rgba(0, 255, 0, 0), #00ff00)");
        styles.set(2, style);
        clear_tree(root);

        auto target = harness.device->create_render_target(target_desc());
        auto commands = harness.device->create_command_list();
        Batched2DConfig config;
        config.enable_aa = aa;
        config.golden_mode = !aa;
        {
            Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                       GlassPipelines{});
            tessera::ui::paint::Batched2DSink sink(renderer);
            renderer.begin(*commands, *target, target_desc());
            (void)tessera::ui::paint::paint_scene(root, styles, sink);
            renderer.end();
            commands->submit();
        }
        const auto pixels = read_target(*harness.device, *target);
        int worst = 0;
        for (std::uint32_t x = 10; x < 54; ++x) {
            const float alpha = (static_cast<float>(x) + 0.5f - 8.3f) / 47.7f;
            const int ideal = static_cast<int>(std::lround(255.0f * alpha));
            worst = std::max(worst, std::abs(pixel_at(pixels, x, 32)[1] - ideal));
        }
        if (worst > 6) {
            std::cerr << "layer_mask_gpu: translucent ramp (aa " << aa << ") is " << worst
                      << " off the ideal ramp\n";
            ok = false;
        }
    }
    return ok;
}

// `--dump` prints the green channel along row 32, column 32 and the diagonal for each
// shape, as JSON, so a Chromium measurement of the same scene can be compared offline.
void dump_masks(Harness& harness) {
    struct Case {
        const char* name;
        const char* css;
        const char* opacity;
    };
    const Case cases[] = {
        {"linear@1", "linear-gradient(90deg, rgba(0,0,0,0), #000)", "1"},
        {"linear@0.5", "linear-gradient(90deg, rgba(0,0,0,0), #000)", "0.5"},
        {"radial@1", "radial-gradient(circle, #000, rgba(0,0,0,0))", "1"},
        {"radial@0.5", "radial-gradient(circle, #000, rgba(0,0,0,0))", "0.5"},
        {"conic@1", "conic-gradient(#000, rgba(0,0,0,0))", "1"},
        {"conic@0.5", "conic-gradient(#000, rgba(0,0,0,0))", "0.5"},
    };
    std::cout << "{";
    bool first = true;
    for (const Case& c : cases) {
        std::uint32_t isolated = 0;
        std::uint32_t applied = 0;
        const auto pixels = render_masked(harness, c.css, c.opacity, isolated, applied);
        const auto series = [&](auto&& at) {
            std::cout << "[";
            for (std::uint32_t i = 0; i < kSize; ++i) {
                std::cout << (i == 0 ? "" : ",") << at(i);
            }
            std::cout << "]";
        };
        std::cout << (first ? "" : ",") << "\"" << c.name << "\":{\"row32\":";
        first = false;
        series([&](std::uint32_t i) { return pixel_at(pixels, i, 32)[1]; });
        std::cout << ",\"col32\":";
        series([&](std::uint32_t i) { return pixel_at(pixels, 32, i)[1]; });
        std::cout << ",\"diag\":";
        series([&](std::uint32_t i) { return pixel_at(pixels, i, i)[1]; });
        std::cout << "}";
    }
    std::cout << "}\n";
}

bool frames_repeat(Harness& harness) {
    // The pooled targets are recycled: the second frame must not see the first one's mask.
    bool ok = masked_pixels(harness);
    ok = masked_pixels(harness) && ok;
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;

    Harness harness;
    try {
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "layer_mask_gpu: skipped (Vulkan device unavailable: " << error.what()
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

    bool ok = masked_pixels(harness);
    ok = mask_composes_with_opacity(harness) && ok;
    ok = without_the_mask_pipeline(harness) && ok;
    ok = frames_repeat(harness) && ok;
    ok = paint_path(harness) && ok;
    ok = chromium_comparison(harness) && ok;
    ok = translucent_gradient_is_composited_once(harness) && ok;
    if (argc > 1 && std::string(argv[1]) == "--dump") {
        dump_masks(harness);
    }

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_mask_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_mask_gpu: ok\n";
    return 0;
}
