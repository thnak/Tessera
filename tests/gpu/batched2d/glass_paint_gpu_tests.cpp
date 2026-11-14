// SPDX-License-Identifier: MIT
//
// The first real glass pixels through the real paint path (issue #244, docs/specs/glass-v1.md
// sections 3-5 and 9). Unlike tests/gpu/batched2d/glass_composite_gpu_tests.cpp, which records the
// composite by hand, this test drives paint_scene -> Batched2DSink -> Batched2DRenderer -> Vulkan:
// the sink's draw_backdrop is the code under test.
//
// Evidence:
//  - a uniform backdrop composites to the analytically expected colour (copy -> blur -> rounded
//    clip -> alpha blend -> node background), so the whole sequence executes in the right place;
//  - a red|blue split under a blur(3px) node produces the separable Gaussian `glass_kernel_taps`
//    computes: every interior pixel of a horizontal scan is compared against the CPU reference
//    derived from the renderer's own region plan (full resolution);
//  - the half-resolution rule above glassMaxBackdropArea still blurs the same edge (the plan
//    reports half resolution and the ramp stays centred and monotonic);
//  - node opacity folds into the composite alpha: out = opacity * blurred + (1 - opacity) *
//  original;
//  - the rounded clip keeps the backdrop outside the corner radius, and nothing outside the node
//  box
//    changes;
//  - `blur(0)` is a pass-through: no backdrop draw at all;
//  - the whole run is validation-clean (layout transitions, descriptor bind and lifetimes).
//
// Skips cleanly when the machine has no Vulkan device.

#include "glass_reference.hpp"
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
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::GlassBackdropRegion;
using tessera::rendering::gpu::GlassKernel;
using tessera::rendering::gpu::GlassLimits;
using tessera::rendering::gpu::GlassPipelines;
using tessera::rendering::gpu::GlassPushConstants;
using tessera::rendering::gpu::GlassTap;
using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

constexpr std::uint32_t kSize = 96;
constexpr std::uint32_t kSplit = 48; // backdrop split: x < kSplit is red, x >= kSplit is blue
constexpr float kBoxX = 24.0f;
constexpr float kBoxY = 24.0f;
constexpr float kBoxSize = 48.0f;
constexpr float kRadius = 8.0f;
constexpr float kSigma = 3.0f;   // CSS px
constexpr float kOpacity = 0.6f; // scene B: folds into the composite alpha
constexpr int kTolerance = 2;    // out of 255

constexpr rhi::Color kRed = rhi::rgba8(255, 0, 0);
constexpr rhi::Color kBlue = rhi::rgba8(0, 0, 255);
// Scene A's uniform backdrop and the glass node's own tint.
constexpr rhi::Color kWall = rhi::rgba8(32, 64, 96);
constexpr float kTintAlpha = 0.25f;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_paint_gpu: " << message << "\n";
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

using glass_reference::Rgb;
using glass_reference::SplitBackdrop;

Rgb to_rgb(const rhi::Color& color) {
    return Rgb{color.r, color.g, color.b};
}

// The scene's backdrop as the shared CPU reference models it.
SplitBackdrop split_backdrop() {
    return SplitBackdrop{to_rgb(kRed), to_rgb(kBlue), static_cast<float>(kSplit)};
}

// The scene: a uniform wall (scene A) or a hard red|blue split (scene B) with one glass node whose
// border box covers the interesting part. The node has no background of its own except in scene A,
// where it also proves the composite is drawn *before* the node's own box visuals.
struct Scene {
    Node root{1};
    StubStyles styles;
    float sigma = 0.0f;

    Scene(bool split, float radius, float scene_sigma, float opacity, bool tinted_background) {
        sigma = scene_sigma;
        root.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
        if (split) {
            auto red = std::make_unique<Node>(2);
            red->set_bounds({0.0f, 0.0f, static_cast<float>(kSplit), static_cast<float>(kSize)});
            root.add_child(std::move(red));
            auto blue = std::make_unique<Node>(3);
            blue->set_bounds({static_cast<float>(kSplit), 0.0f, static_cast<float>(kSize - kSplit),
                              static_cast<float>(kSize)});
            root.add_child(std::move(blue));
            styles.set(2, styled({{"background-color", "#ff0000"}}));
            styles.set(3, styled({{"background-color", "#0000ff"}}));
        } else {
            styles.set(1, styled({{"background-color", "#204060"}}));
        }

        auto glass = std::make_unique<Node>(4);
        glass->set_bounds({kBoxX, kBoxY, kBoxSize, kBoxSize});
        root.add_child(std::move(glass));

        // The filter value the paint pass parses; `blur(0)` is a pass-through declaration.
        const std::string filter =
            sigma > 0.0f ? "blur(" + std::to_string(static_cast<int>(sigma)) + "px)" : "blur(0)";
        std::string opacity_value;
        if (opacity != 1.0f) {
            // opacity is a number in this engine (a percentage is invalid and falls back to 1).
            opacity_value = std::to_string(opacity);
        }
        // The paint pass reads the four radius longhands (the cascade expands the `border-radius`
        // shorthand; a hand-built ComputedStyle has to spell them out).
        std::string radius_value;
        if (radius > 0.0f) {
            radius_value = std::to_string(static_cast<int>(radius)) + "px";
        }
        ComputedStyle glass_style;
        for (const char* corner : {"border-top-left-radius", "border-top-right-radius",
                                   "border-bottom-right-radius", "border-bottom-left-radius"}) {
            glass_style.set(corner, radius_value.empty() ? "0px" : radius_value);
        }
        glass_style.set("backdrop-filter", filter);
        glass_style.set("opacity", opacity_value.empty() ? "1" : opacity_value);
        if (tinted_background) {
            glass_style.set("background-color", "rgba(255, 255, 255, 0.25)");
        }
        styles.set(4, std::move(glass_style));
        clear_tree(root);
    }
};

// One rendered frame through the paint path, returning the RGBA pixels and the paint/glass stats.
struct Frame {
    std::vector<std::uint8_t> pixels;
    PaintStats paint;
    tessera::ui::paint::GlassPaintStats glass;
    GlassBackdropRegion region;
};

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
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

std::array<std::uint8_t, 4> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

bool close_to(int actual, float expected_unit, int tolerance, const char* what, std::uint32_t x,
              std::uint32_t y) {
    const int target =
        static_cast<int>(std::lround(std::clamp(expected_unit, 0.0f, 1.0f) * 255.0f));
    if (std::abs(actual - target) <= tolerance) {
        return true;
    }
    std::cerr << "glass_paint_gpu: " << what << " at (" << x << "," << y << "): got " << actual
              << ", expected " << target << "\n";
    return false;
}

Frame render(Harness& harness, Scene& scene, const GlassLimits& limits) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false; // hard edges: the rounded clip is the only AA in these scenes
    config.golden_mode = true;
    config.glass = limits;
    {
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config,
                                   GlassPipelines{harness.blur.get(), harness.composite.get()});
        Batched2DSink sink(renderer);
        PaintContext context;
        context.glass = limits;
        renderer.begin(*commands, *target, target_desc());
        Frame frame;
        frame.paint = paint_scene(scene.root, scene.styles, sink, context);
        renderer.end();
        frame.glass = sink.glass_stats();
        commands->submit();
        harness.device->wait_idle();
        frame.pixels = read_target(*harness.device, *target);
        // The region the renderer used, from the same planner: the CPU reference below is derived
        // from it instead of a duplicated formula.
        frame.region = tessera::rendering::gpu::plan_glass_backdrop_region(
            rhi::Rect{kBoxX, kBoxY, kBoxSize, kBoxSize}, scene.sigma, 1.0f, limits, {kSize, kSize});
        return frame;
    }
}

// --- scene A: a uniform backdrop, so the blur is the identity and the expected colour is analytic.
bool uniform_backdrop_composites_exactly(Harness& harness) {
    Scene scene(false, kRadius, kSigma, 1.0f, true);
    const GlassLimits limits;
    const Frame frame = render(harness, scene, limits);
    bool ok = expect(frame.paint.backdrops == 1, "the paint pass emitted one backdrop") &&
              expect(frame.glass.draws == 1, "the sink executed the composite") &&
              expect(frame.region.active, "the renderer planned a region");
    const Rgb wall = to_rgb(kWall);
    // The node's own 25% white background paints over the composited (uniform) backdrop.
    const float expected_r = wall.r * (1.0f - kTintAlpha) + 1.0f * kTintAlpha;
    const float expected_g = wall.g * (1.0f - kTintAlpha) + 1.0f * kTintAlpha;
    const float expected_b = wall.b * (1.0f - kTintAlpha) + 1.0f * kTintAlpha;
    for (std::uint32_t x = 36; x < 60; ++x) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 48);
        ok = close_to(pixel[0], expected_r, kTolerance, "uniform red", x, 48) &&
             close_to(pixel[1], expected_g, kTolerance, "uniform green", x, 48) &&
             close_to(pixel[2], expected_b, kTolerance, "uniform blue", x, 48) && ok;
    }
    // Evidence line for the CTest log: the analytic composite against the pixel the paint path
    // produced.
    const auto to_byte = [](float v) { return static_cast<int>(v * 255.0f + 0.5f); };
    const auto inside = pixel_at(frame.pixels, 48, 48);
    std::printf("glass_paint_gpu: uniform expected %d/%d/%d got %u/%u/%u\n", to_byte(expected_r),
                to_byte(expected_g), to_byte(expected_b), inside[0], inside[1], inside[2]);
    // Outside the node box the wall is untouched (and the glass node's tint never reaches it).
    for (const std::array<std::uint32_t, 2>& point :
         {std::array<std::uint32_t, 2>{4, 48}, std::array<std::uint32_t, 2>{48, 4},
          std::array<std::uint32_t, 2>{90, 90}}) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, point[0], point[1]);
        ok = close_to(pixel[0], wall.r, 1, "wall red outside the box", point[0], point[1]) &&
             close_to(pixel[1], wall.g, 1, "wall green outside the box", point[0], point[1]) &&
             close_to(pixel[2], wall.b, 1, "wall blue outside the box", point[0], point[1]) && ok;
    }
    return ok;
}

// --- scene B: the red|blue split, compared pixel by pixel against the CPU kernel reference, with
// the node opacity folded in: out = opacity * blurred + (1 - opacity) * original.
bool split_backdrop_matches_the_kernel(Harness& harness) {
    Scene scene(true, 0.0f, kSigma, kOpacity, false);
    const GlassLimits limits;
    const Frame frame = render(harness, scene, limits);
    bool ok =
        expect(frame.paint.backdrops == 1, "the split scene emits one backdrop") &&
        expect(frame.glass.draws == 1, "the split composite executed") &&
        expect(!frame.region.half_resolution, "the region is small enough for full resolution");
    if (!ok) {
        return false;
    }
    // Interior columns of the node: coverage is 1, so the composite is exactly the opacity blend.
    for (std::uint32_t x = 25; x < 71; ++x) {
        const Rgb blurred = glass_reference::blurred(frame.region, split_backdrop(), x);
        const Rgb original = split_backdrop().texel(x);
        const float expected[3] = {kOpacity * blurred.r + (1.0f - kOpacity) * original.r,
                                   kOpacity * blurred.g + (1.0f - kOpacity) * original.g,
                                   kOpacity * blurred.b + (1.0f - kOpacity) * original.b};
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 48);
        ok = close_to(pixel[0], expected[0], kTolerance, "blurred red", x, 48) &&
             close_to(pixel[1], expected[1], kTolerance, "blurred green", x, 48) &&
             close_to(pixel[2], expected[2], kTolerance, "blurred blue", x, 48) && ok;
    }
    // The edge is a ramp, not a step: red falls and blue rises monotonically through the split.
    std::uint8_t previous_red = 255;
    std::uint8_t previous_blue = 0;
    for (std::uint32_t x = 36; x < 61; ++x) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 48);
        ok = expect(pixel[0] <= previous_red, "the blurred edge fades red monotonically") && ok;
        ok =
            expect(pixel[2] >= previous_blue, "the blurred edge rises in blue monotonically") && ok;
        previous_red = pixel[0];
        previous_blue = pixel[2];
    }
    // Nothing outside the node box changes.
    for (std::uint32_t x = 0; x < kSize; x += 3) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 8);
        const std::array<std::uint8_t, 4> expected_pixel =
            x < kSplit ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                       : std::array<std::uint8_t, 4>{0, 0, 255, 255};
        ok = expect(pixel == expected_pixel, "pixels outside the node keep the backdrop") && ok;
    }
    return ok;
}

// --- the half-resolution rule: above glassMaxBackdropArea the blur target is half the region and
// the vertical pass upsamples, but the same edge still blurs around the same place.
bool half_resolution_blurs_the_same_edge(Harness& harness) {
    Scene scene(true, 0.0f, kSigma, 1.0f, false);
    GlassLimits limits;
    limits.max_backdrop_area = 1000; // far below the 66x66 dependency region
    const Frame frame = render(harness, scene, limits);
    bool ok = expect(frame.region.active && frame.region.half_resolution,
                     "the area limit switches the blur target to half resolution") &&
              expect(frame.region.blur.width == (frame.region.source.width + 1) / 2,
                     "the blur target is half the region") &&
              expect(frame.glass.draws == 1, "the half-resolution composite executed");
    if (!ok) {
        return false;
    }
    // The ramp is centred on the split and monotonic, and the far sides are the pure colours.
    const std::array<std::uint8_t, 4> left = pixel_at(frame.pixels, 30, 48);
    const std::array<std::uint8_t, 4> right = pixel_at(frame.pixels, 66, 48);
    ok = expect(left[0] > 200 && left[2] < 40, "the far left of the node stays red") &&
         expect(right[2] > 200 && right[0] < 40, "the far right of the node stays blue") && ok;
    std::uint8_t previous_red = 255;
    for (std::uint32_t x = 40; x < 57; ++x) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 48);
        ok = expect(pixel[0] <= previous_red,
                    "the half-resolution edge still fades monotonically") &&
             ok;
        previous_red = pixel[0];
    }
    // The mid-point of the split sits at the split within one pixel (the ramp is symmetric).
    const std::array<std::uint8_t, 4> at_split = pixel_at(frame.pixels, kSplit - 1, 48);
    const std::array<std::uint8_t, 4> after_split = pixel_at(frame.pixels, kSplit, 48);
    ok = expect(at_split[0] > 40 && at_split[2] > 40 && after_split[0] > 40 && after_split[2] > 40,
                "the split is blended, not a hard step") &&
         ok;
    return ok;
}

// --- the rounded clip: outside the corner radius the backdrop shows, inside the box it composites.
bool rounded_clip_keeps_the_backdrop_at_the_corners(Harness& harness) {
    Scene scene(false, kRadius, kSigma, 1.0f, true);
    const GlassLimits limits;
    const Frame frame = render(harness, scene, limits);
    const Rgb wall = to_rgb(kWall);
    bool ok = expect(frame.glass.draws == 1, "the rounded node composites");
    // The very corner pixel of the box is outside the 8px radius: the coverage is 0, so the
    // composite writes nothing and the backdrop (uniform wall) is what is left.
    for (const std::array<std::uint32_t, 2>& point :
         {std::array<std::uint32_t, 2>{24, 24}, std::array<std::uint32_t, 2>{71, 24},
          std::array<std::uint32_t, 2>{24, 71}}) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, point[0], point[1]);
        ok = close_to(pixel[0], wall.r, 1, "corner red shows the backdrop", point[0], point[1]) &&
             close_to(pixel[1], wall.g, 1, "corner green shows the backdrop", point[0], point[1]) &&
             close_to(pixel[2], wall.b, 1, "corner blue shows the backdrop", point[0], point[1]) &&
             ok;
    }
    // The box interior is still composited with the node tint.
    const float expected_r = wall.r * (1.0f - kTintAlpha) + kTintAlpha;
    const std::array<std::uint8_t, 4> inside = pixel_at(frame.pixels, 48, 48);
    ok = close_to(inside[0], expected_r, kTolerance, "the interior is tinted", 48, 48) && ok;
    return ok;
}

// --- `blur(0)` is a pass-through: the paint pass emits no backdrop at all (glass-v1 section 2).
bool blur_zero_is_a_pass_through(Harness& harness) {
    Scene scene(true, 0.0f, 0.0f, 1.0f, false);
    const GlassLimits limits;
    const Frame frame = render(harness, scene, limits);
    bool ok = expect(frame.paint.backdrops == 0, "blur(0) emits no backdrop") &&
              expect(frame.glass.draws == 0 && frame.glass.unsupported == 0,
                     "the sink sees no glass draw") &&
              expect(!frame.region.active, "the renderer plans no region");
    for (std::uint32_t x = 0; x < kSize; x += 8) {
        const std::array<std::uint8_t, 4> pixel = pixel_at(frame.pixels, x, 48);
        const std::array<std::uint8_t, 4> expected_pixel =
            x < kSplit ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                       : std::array<std::uint8_t, 4>{0, 0, 255, 255};
        ok = expect(pixel == expected_pixel, "a pass-through node shows the backdrop untouched") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    Harness harness;
    try {
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "glass_paint_gpu: skipped (Vulkan device unavailable: " << error.what()
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

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = uniform_backdrop_composites_exactly(harness);
    ok = split_backdrop_matches_the_kernel(harness) && ok;
    ok = half_resolution_blurs_the_same_edge(harness) && ok;
    ok = rounded_clip_keeps_the_backdrop_at_the_corners(harness) && ok;
    ok = blur_zero_is_a_pass_through(harness) && ok;

    // The layout transitions, descriptor binds and pooled-target lifetimes this test drives must be
    // validation-clean: a pixel cannot prove that.
    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;

    if (!ok) {
        std::cerr << "glass_paint_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "glass_paint_gpu: ok\n";
    return 0;
}
