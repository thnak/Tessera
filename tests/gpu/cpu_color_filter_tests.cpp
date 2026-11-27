// SPDX-License-Identifier: MIT
//
// The CSS colour filter functions on the CPU twin (issue #617): `CpuPaintSink` runs a layer's
// `ColorFunctions` ops on its black/white canvas pair (cpu_layer.cpp apply_layer_color_filter).
// Vulkan-free. Two kinds of evidence, both through the real paint pass (`filter: <css>` on a node):
//  - every Chromium sample of tests/fixtures/chromium_color_filter.hpp (opaque colours and
//    translucent ones over black) reproduces within 3/255, the slack of the 8-bit pair a
//    translucent layer is stored in;
//  - the filter precedes the mask and the group opacity, and runs once.

#include "chromium_color_filter.hpp"
#include "cpu_wide_scene_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;
namespace chromium = tessera::fixtures::chromium_filter;
using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::gpu::ColorFunction;
using tessera::rendering::gpu::LayerFilterChain;
using tessera::rendering::gpu::LayerFilterOp;
using tessera::rendering::rhi::Color;
using tessera::ui::paint::LayerDraw;

constexpr tessera::rendering::rhi::Rect kFrame{0.0f, 0.0f, 8.0f, 2.0f};
constexpr int kTolerance = 3;

CpuCanvas black_page() {
    CpuCanvas canvas(8, 2);
    canvas.clear(Color{0.0f, 0.0f, 0.0f, 1.0f});
    return canvas;
}

// A node at (0,0) 8x2 with the given background and `filter`, painted onto black.
CpuCanvas paint_filtered(const std::string& background, const char* css) {
    Node root(1);
    StubStyles styles;
    root.set_bounds(kFrame);
    add_node(root, 2, {0, 0, 8, 2});
    styles.set(2, styled({{"background-color", background.c_str()}, {"filter", css}}));
    CpuCanvas canvas = black_page();
    CpuPaintSink sink(canvas);
    tessera::ui::paint::paint_scene(root, styles, sink);
    return canvas;
}

std::string rgb_text(const std::array<int, 3>& value, float alpha) {
    return "rgba(" + std::to_string(value[0]) + "," + std::to_string(value[1]) + "," +
           std::to_string(value[2]) + "," + std::to_string(alpha) + ")";
}

bool near(const CpuCanvas& canvas, const char* css, const std::array<int, 3>& want,
          const std::string& what) {
    const Pixel actual = pixel(canvas, 3, 1);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(static_cast<int>(actual[channel]) - want[channel]) > kTolerance) {
            std::cerr << "cpu_color_filter: " << css << " on " << what << ": got ("
                      << int{actual[0]} << "," << int{actual[1]} << "," << int{actual[2]}
                      << "), Chromium (" << want[0] << "," << want[1] << "," << want[2] << ")\n";
            return false;
        }
    }
    return true;
}

bool matches_chromium() {
    bool ok = true;
    for (const chromium::OpaqueSample& sample : chromium::kOpaque) {
        ok = near(paint_filtered(rgb_text(sample.input, 1.0f), sample.css), sample.css,
                  sample.output, rgb_text(sample.input, 1.0f)) &&
             ok;
    }
    for (const chromium::AlphaSample& sample : chromium::kAlpha) {
        ok = near(paint_filtered(rgb_text(sample.input, sample.alpha), sample.css), sample.css,
                  sample.over_black, rgb_text(sample.input, sample.alpha)) &&
             ok;
    }
    return ok;
}

LayerFilterChain invert_chain() {
    LayerFilterChain chain;
    LayerFilterOp op;
    op.set_color_function(0, ColorFunction::Invert, 1.0f);
    chain.push(op);
    return chain;
}

// The filter sees the whole layer and runs before the mask, once: an inverted red layer masked to
// its left half is cyan there and a hole elsewhere, and the page shows through the hole.
bool filter_precedes_mask_and_opacity() {
    CpuCanvas canvas = black_page();
    CpuPaintSink sink(canvas);
    sink.push_layer(LayerDraw{kFrame, 0.5f, invert_chain()});
    sink.fill_rect(kFrame, Color{1.0f, 0.0f, 0.0f, 1.0f});
    bool ok = expect(sink.begin_layer_mask(), "the mask is accepted");
    sink.fill_rect({0, 0, 4, 2}, Color{0.0f, 0.0f, 0.0f, 1.0f});
    sink.end_layer_mask();
    sink.pop_layer();
    // Inverted red is cyan, at the group's 0.5 over black: (0, 128, 128). The unmasked half is a
    // hole, so it keeps the page.
    const Pixel left = pixel(canvas, 1, 0);
    const Pixel right = pixel(canvas, 6, 0);
    ok = expect(std::abs(left[0] - 0) <= 2 && std::abs(left[1] - 128) <= 2 &&
                    std::abs(left[2] - 128) <= 2,
                "masked half: inverted, then faded") &&
         ok;
    ok = expect(right[0] == 0 && right[1] == 0 && right[2] == 0, "unmasked half: the page") && ok;
    return ok;
}

// A transparent layer stays transparent whatever the functions add (invert offsets, contrast).
bool undrawn_pixels_stay_transparent() {
    CpuCanvas canvas(8, 2);
    canvas.clear(Color{0.0f, 0.0f, 1.0f, 1.0f});
    CpuPaintSink sink(canvas);
    LayerFilterChain chain = invert_chain();
    LayerFilterOp contrast;
    contrast.set_color_function(0, ColorFunction::Contrast, 0.0f);
    chain.push(contrast);
    sink.push_layer(LayerDraw{kFrame, 1.0f, chain});
    sink.fill_rect({0, 0, 2, 2}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    sink.pop_layer();
    const Pixel hole = pixel(canvas, 5, 0);
    const Pixel drawn = pixel(canvas, 0, 0);
    return expect(hole[0] == 0 && hole[1] == 0 && hole[2] == 255,
                  "an undrawn pixel keeps the page") &&
           expect(std::abs(drawn[0] - 128) <= 1 && std::abs(drawn[1] - 128) <= 1 &&
                      std::abs(drawn[2] - 128) <= 1,
                  "invert then contrast(0) is mid grey where drawn");
}

} // namespace

int main() {
    bool ok = matches_chromium();
    ok = filter_precedes_mask_and_opacity() && ok;
    ok = undrawn_pixels_stay_transparent() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "cpu_color_filter: ok\n";
    return 0;
}
