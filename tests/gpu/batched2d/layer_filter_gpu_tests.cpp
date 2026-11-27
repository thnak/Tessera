// SPDX-License-Identifier: MIT
//
// Layer filters (issue #617, docs/design/gpu/offscreen-layers.md section 5.2) on a real device: the
// pixels the `blur()` and `drop-shadow()` paths must produce.
//
// Evidence:
//  - a blurred red box matches the analytic Gaussian convolution of a box (the shader's tap pairs
//    are exact for the Gaussian, so the only error is the 3 sigma cut-off and 8-bit rounding);
//  - a drop-shadow is the box's alpha, offset, blurred and tinted, under the unmoved box: the
//  shadow
//    shows only outside the box, the box stays opaque, the tint's alpha scales the shadow;
//  - the group opacity applies after the filter, to the box and its shadow together;
//  - the filter runs before the mask: a blurred layer under a half-plane mask is cut exactly at the
//    mask edge instead of bleeding past it;
//  - the same scenes measured in Chromium (canvas `filter`, the CSS filter pipeline) agree;
//  - the real paint path: `filter: blur(2px)` and `filter: drop-shadow(...)` on a node, through
//    Batched2DSink, and the unfiltered fallback without the blur pipeline;
//  - recycled pooled targets leave no residue between frames, and the run is validation-clean.
//
// Skips cleanly when the machine has no Vulkan device.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

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
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 4; // out of 255: the 3 sigma cut-off plus 8-bit rounding of two passes
constexpr rhi::Color kRed = rhi::rgba8(255, 0, 0);
constexpr rhi::Rect kBox{24.0f, 24.0f, 16.0f, 16.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layer_filter_gpu: " << message << "\n";
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

using Pixels = std::vector<std::uint8_t>;

Pixels read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    Pixels pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

int channel(const Pixels& pixels, std::uint32_t x, std::uint32_t y, std::size_t c) {
    return pixels[(static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t) + c];
}

bool near(const Pixels& pixels, std::uint32_t x, std::uint32_t y, int r, int g, int b,
          const char* what) {
    if (std::abs(channel(pixels, x, y, 0) - r) <= kTolerance &&
        std::abs(channel(pixels, x, y, 1) - g) <= kTolerance &&
        std::abs(channel(pixels, x, y, 2) - b) <= kTolerance) {
        return true;
    }
    std::cerr << "layer_filter_gpu: " << what << " at (" << x << "," << y << "): got ("
              << channel(pixels, x, y, 0) << "," << channel(pixels, x, y, 1) << ","
              << channel(pixels, x, y, 2) << "), expected (" << r << "," << g << "," << b << ")\n";
    return false;
}

// Coverage of the pixel centred at `position + 0.5` by [lo, hi) blurred with a Gaussian of
// `sigma`; sigma 0 is the hard box. The 2D box factors, so the product of two of these is the
// blurred rectangle.
float coverage(float position, float lo, float hi, float sigma) {
    const float centre = position + 0.5f;
    if (sigma <= 0.0f) {
        return centre >= lo && centre < hi ? 1.0f : 0.0f;
    }
    const float scale = sigma * std::sqrt(2.0f);
    return 0.5f * (std::erf((hi - centre) / scale) - std::erf((lo - centre) / scale));
}

// Expected alpha of the box blurred by `sigma` and moved by (dx, dy), at pixel (x, y).
float box_alpha(std::uint32_t x, std::uint32_t y, float sigma, float dx = 0.0f, float dy = 0.0f) {
    return coverage(static_cast<float>(x) - dx, kBox.x, kBox.right(), sigma) *
           coverage(static_cast<float>(y) - dy, kBox.y, kBox.bottom(), sigma);
}

int scaled(float alpha, float factor = 1.0f) {
    return static_cast<int>(std::lround(255.0f * alpha * factor));
}

LayerFilterChain blur_chain(float sigma) {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.sigma = sigma;
    chain.push(op);
    return chain;
}

LayerFilterChain shadow_chain(float dx, float dy, float sigma, rhi::Color color) {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.kind = LayerFilterOp::Kind::DropShadow;
    op.offset_x = dx;
    op.offset_y = dy;
    op.sigma = sigma;
    op.set_tint(color);
    chain.push(op);
    return chain;
}

struct Frame {
    Pixels pixels;
    std::uint32_t applied = 0;
    std::uint32_t unsupported = 0;
};

// A red box layer at `opacity` with `filter`; with `mask_edge` set, masked by a white half-plane
// x < mask_edge.
Frame render(Harness& harness, const LayerFilterChain& filter, float opacity = 1.0f,
             float mask_edge = -1.0f, bool with_blur = true, bool with_mask = true) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        const GlassPipelines pipelines{with_blur ? harness.blur.get() : nullptr,
                                       harness.composite.get(),
                                       with_mask ? harness.mask.get() : nullptr};
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   pipelines);
        renderer.begin(*commands, *target, target_desc());
        renderer.push_layer({0.0f, 0.0f, 64.0f, 64.0f}, opacity, filter);
        renderer.push_rect(kBox, kRed);
        if (mask_edge >= 0.0f && renderer.begin_layer_mask()) {
            renderer.push_rect({0.0f, 0.0f, mask_edge, 64.0f}, rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
            renderer.end_layer_mask();
        }
        renderer.pop_layer();
        renderer.end();
        commands->submit();
        frame.applied = renderer.layer_filters_applied();
        frame.unsupported = renderer.layer_filters_unsupported();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

bool blur_pixels(Harness& harness) {
    constexpr float kSigma = 2.0f;
    const Frame frame = render(harness, blur_chain(kSigma));
    bool ok = expect(frame.applied == 1 && frame.unsupported == 0, "the blur was applied");
    for (const std::uint32_t x : {16u, 20u, 22u, 24u, 26u, 31u, 38u, 41u, 44u, 48u}) {
        ok = near(frame.pixels, x, 32, scaled(box_alpha(x, 32, kSigma)), 0, 0,
                  "blurred box, row 32") &&
             ok;
    }
    for (const std::uint32_t y : {18u, 22u, 24u, 40u, 43u}) {
        ok = near(frame.pixels, 31, y, scaled(box_alpha(31, y, kSigma)), 0, 0,
                  "blurred box, column 31") &&
             ok;
    }
    // The corner is the product of both axes' falloff, the part a one-axis blur would get wrong.
    ok =
        near(frame.pixels, 22, 22, scaled(box_alpha(22, 22, kSigma)), 0, 0, "blurred corner") && ok;
    ok = near(frame.pixels, 3, 3, 0, 0, 0, "far from the box is untouched") && ok;
    // Hard-edged control: the same scene without a filter has a sharp edge, so the blur is real.
    const Frame sharp = render(harness, LayerFilterChain{});
    return expect(channel(sharp.pixels, 22, 32, 0) == 0 && channel(sharp.pixels, 24, 32, 0) == 255,
                  "without a filter the edge is sharp") &&
           ok;
}

bool larger_sigma_than_the_frame_edge(Harness& harness) {
    // sigma 12 reaches 36 px, past the frame on every side of the box: a clamped kernel must still
    // conserve the centre value and stay inside the frame without a validation message.
    const Frame frame = render(harness, blur_chain(12.0f));
    return expect(frame.applied == 1, "the wide blur was applied") &&
           near(frame.pixels, 32, 32, scaled(box_alpha(32, 32, 12.0f)), 0, 0, "wide blur centre");
}

bool drop_shadow_pixels(Harness& harness) {
    const rhi::Color blue{0.0f, 0.0f, 1.0f, 0.5f};
    const Frame hard = render(harness, shadow_chain(8.0f, 6.0f, 0.0f, blue));
    bool ok = expect(hard.applied == 1 && hard.unsupported == 0, "the shadow was applied");
    ok = near(hard.pixels, 32, 32, 255, 0, 0, "the box stays opaque over its shadow") && ok;
    ok = near(hard.pixels, 44, 42, 0, 0, 128, "shadow-only pixel is the half-alpha tint") && ok;
    ok = near(hard.pixels, 36, 44, 0, 0, 128, "shadow below the box") && ok;
    ok = near(hard.pixels, 44, 26, 0, 0, 0, "above the shifted shadow nothing shows") && ok;
    ok = near(hard.pixels, 10, 10, 0, 0, 0, "away from both") && ok;

    constexpr float kSigma = 2.0f;
    const Frame soft = render(harness, shadow_chain(8.0f, 6.0f, kSigma, blue));
    for (const std::uint32_t x : {36u, 42u, 44u, 46u, 50u}) {
        // Beyond the box (x >= 40 or y >= 40) the shadow is all that shows.
        ok = near(soft.pixels, x, 44, 0, 0, scaled(box_alpha(x, 44, kSigma, 8.0f, 6.0f), 0.5f),
                  "soft shadow along row 44") &&
             ok;
    }
    ok = near(soft.pixels, 32, 32, 255, 0, 0, "the soft shadow stays under the box") && ok;
    return ok;
}

bool opacity_applies_after_the_filter(Harness& harness) {
    const rhi::Color blue{0.0f, 0.0f, 1.0f, 0.5f};
    const Frame frame = render(harness, shadow_chain(8.0f, 6.0f, 0.0f, blue), 0.5f);
    bool ok = near(frame.pixels, 32, 32, 128, 0, 0, "the box fades to half");
    ok = near(frame.pixels, 44, 42, 0, 0, 64, "the shadow fades with it: 0.5 x 0.5") && ok;
    return ok;
}

bool filter_runs_before_the_mask(Harness& harness) {
    constexpr float kSigma = 2.0f;
    const Frame frame = render(harness, blur_chain(kSigma), 1.0f, 32.0f);
    bool ok = expect(frame.applied == 1, "the blur was applied");
    ok = near(frame.pixels, 30, 32, scaled(box_alpha(30, 32, kSigma)), 0, 0,
              "left of the mask edge keeps the blurred box") &&
         ok;
    // Masking before blurring would let the blurred box leak across x = 32; filter first cuts it.
    ok = near(frame.pixels, 33, 32, 0, 0, 0, "right of the mask edge is a hole") && ok;
    ok = near(frame.pixels, 38, 32, 0, 0, 0, "and stays one") && ok;
    return ok;
}

bool fallbacks_paint_the_box_unfiltered(Harness& harness) {
    const Frame no_blur = render(harness, blur_chain(2.0f), 1.0f, -1.0f, false);
    bool ok = expect(no_blur.applied == 0 && no_blur.unsupported == 1, "no blur pipeline: counted");
    ok = near(no_blur.pixels, 22, 32, 0, 0, 0, "unfiltered: sharp edge outside") && ok;
    ok = near(no_blur.pixels, 24, 32, 255, 0, 0, "unfiltered: sharp edge inside") && ok;
    const Frame no_mask = render(harness, shadow_chain(8.0f, 6.0f, 0.0f, rhi::Color{0, 0, 1, 1}),
                                 1.0f, -1.0f, true, false);
    ok = expect(no_mask.unsupported == 1, "no mask pipeline: a shadow is counted") && ok;
    ok = near(no_mask.pixels, 44, 42, 0, 0, 0, "and paints no shadow") && ok;
    return ok;
}

// The same scenes Chromium measured (a 64x64 black canvas, a #ff0000 box at (24,24) 16x16, drawn
// with `ctx.filter = ...`, which shares the CSS filter pipeline; measured 2026-11-27): the renderer
// agrees within the tolerance, which also pins that a drop-shadow's blur is a standard deviation
// (a box-shadow-style radius of twice that would be visibly wider).
bool chromium_comparison(Harness& harness) {
    struct Sample {
        std::uint32_t x;
        std::uint32_t y;
        int value;
    };
    const Frame blurred = render(harness, blur_chain(2.0f));
    const Sample blur_samples[] = {{16, 32, 0},   {20, 32, 10},  {22, 32, 57},  {24, 32, 153},
                                   {26, 32, 229}, {31, 32, 255}, {38, 32, 198}, {41, 32, 57},
                                   {44, 32, 3},   {48, 32, 0},   {22, 22, 13}};
    bool ok = true;
    for (const Sample& sample : blur_samples) {
        ok =
            near(blurred.pixels, sample.x, sample.y, sample.value, 0, 0, "blur(2px) vs Chromium") &&
            ok;
    }
    const Frame shadowed =
        render(harness, shadow_chain(8.0f, 6.0f, 2.0f, rhi::Color{0.0f, 0.0f, 1.0f, 0.5f}));
    const Sample shadow_samples[] = {
        {36, 44, 98}, {42, 44, 99}, {44, 44, 95}, {46, 44, 77}, {50, 44, 10}};
    for (const Sample& sample : shadow_samples) {
        ok = near(shadowed.pixels, sample.x, sample.y, 0, 0, sample.value,
                  "drop-shadow(8px 6px 2px) vs Chromium") &&
             ok;
    }
    return near(shadowed.pixels, 32, 32, 255, 0, 0, "the box is opaque over its shadow") && ok;
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

// A red 16x16 node at (24,24) with `filter: <css>`, painted through the real paint path.
Frame render_styled(Harness& harness, const char* css, bool with_blur = true) {
    using tessera::ui::Node;
    using tessera::ui::style::ComputedStyle;
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 64.0f, 64.0f});
    auto node = std::make_unique<Node>(2);
    node->set_bounds(kBox);
    root.add_child(std::move(node));
    StubStyles styles;
    ComputedStyle style;
    style.set("background-color", "#ff0000");
    style.set("filter", css);
    styles.set(2, style);
    clear_tree(root);

    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   GlassPipelines{with_blur ? harness.blur.get() : nullptr,
                                                  harness.composite.get(), harness.mask.get()});
        tessera::ui::paint::Batched2DSink sink(renderer);
        renderer.begin(*commands, *target, target_desc());
        (void)tessera::ui::paint::paint_scene(root, styles, sink);
        renderer.end();
        commands->submit();
        frame.applied = renderer.layer_filters_applied();
        frame.unsupported = renderer.layer_filters_unsupported();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

bool paint_path(Harness& harness) {
    constexpr float kSigma = 2.0f;
    const Frame blurred = render_styled(harness, "blur(2px)");
    bool ok = expect(blurred.applied == 1, "the paint pass opened one filtered layer");
    for (const std::uint32_t x : {20u, 23u, 24u, 31u, 41u}) {
        ok = near(blurred.pixels, x, 32, scaled(box_alpha(x, 32, kSigma)), 0, 0,
                  "blur(2px) through the paint path") &&
             ok;
    }
    const Frame shadowed = render_styled(harness, "drop-shadow(8px 6px 0 rgba(0, 0, 255, 0.5))");
    ok = expect(shadowed.applied == 1, "the paint pass opened one shadowed layer") && ok;
    ok = near(shadowed.pixels, 32, 32, 255, 0, 0, "box over its shadow") && ok;
    ok = near(shadowed.pixels, 44, 42, 0, 0, 128, "the shadow through the paint path") && ok;
    const Frame chained = render_styled(harness, "blur(1px) drop-shadow(10px 0 0 #00ff00)");
    ok = expect(chained.applied == 2, "a two-function chain runs both") && ok;
    ok = near(chained.pixels, 46, 32, 0, 255, 0, "the second function sees the first's output") &&
         ok;
    const Frame unfiltered = render_styled(harness, "blur(2px)", false);
    ok = expect(unfiltered.unsupported == 1, "without the blur pipeline the node still paints") &&
         ok;
    ok = near(unfiltered.pixels, 24, 32, 255, 0, 0, "unfiltered box") && ok;
    return ok;
}

bool frames_repeat(Harness& harness) {
    // Pooled targets are recycled and cleared: a later frame with another filter must not see the
    // earlier frame's blur or shadow.
    bool ok = blur_pixels(harness);
    ok = drop_shadow_pixels(harness) && ok;
    const Frame plain = render(harness, LayerFilterChain{});
    ok = near(plain.pixels, 44, 42, 0, 0, 0, "no residue of an earlier shadow") && ok;
    ok = near(plain.pixels, 22, 32, 0, 0, 0, "no residue of an earlier blur") && ok;
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
        std::cout << "layer_filter_gpu: skipped (Vulkan device unavailable: " << error.what()
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

    bool ok = blur_pixels(harness);
    ok = larger_sigma_than_the_frame_edge(harness) && ok;
    ok = drop_shadow_pixels(harness) && ok;
    ok = opacity_applies_after_the_filter(harness) && ok;
    ok = filter_runs_before_the_mask(harness) && ok;
    ok = fallbacks_paint_the_box_unfiltered(harness) && ok;
    ok = chromium_comparison(harness) && ok;
    ok = paint_path(harness) && ok;
    ok = frames_repeat(harness) && ok;

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "layer_filter_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "layer_filter_gpu: ok\n";
    return 0;
}
