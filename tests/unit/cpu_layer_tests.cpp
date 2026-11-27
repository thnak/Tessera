// SPDX-License-Identifier: MIT
//
// The CPU twin of isolated layers and layer masks (issue #617): alpha recovered from the black and
// white canvas pair, the composite, nesting, and the mask step, on exact RGBA8 bytes. No GPU.

#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/rendering/software/cpu_layer.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using tessera::rendering::gpu::MaskCompositeOp;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::rendering::software::apply_layer_mask;
using tessera::rendering::software::combine_mask_part;
using tessera::rendering::software::composite_layer;
using tessera::rendering::software::CpuCanvas;

constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Rect kAll{0.0f, 0.0f, 4.0f, 1.0f};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cpu_layer: " << message << "\n";
    }
    return condition;
}

std::array<int, 3> pixel(const CpuCanvas& canvas, std::uint32_t x) {
    const std::size_t index = static_cast<std::size_t>(x) * 4;
    return {canvas.pixels()[index], canvas.pixels()[index + 1], canvas.pixels()[index + 2]};
}

// A layer pair: both canvases cleared, then the same draws applied to each.
struct Pair {
    CpuCanvas black{4, 1};
    CpuCanvas white{4, 1};
    Pair() {
        black.clear(kBlack);
        white.clear(kWhite);
    }
    void fill(const Rect& rect, Color color) {
        black.fill_rect(rect, color);
        white.fill_rect(rect, color);
    }
};

CpuCanvas page() {
    CpuCanvas canvas(4, 1);
    canvas.clear(Color{0.0f, 0.0f, 1.0f, 1.0f}); // blue
    return canvas;
}

bool near(const std::array<int, 3>& got, int r, int g, int b, int tolerance) {
    return std::abs(got[0] - r) <= tolerance && std::abs(got[1] - g) <= tolerance &&
           std::abs(got[2] - b) <= tolerance;
}

bool test_opaque_and_hole() {
    Pair layer;
    layer.fill({0.0f, 0.0f, 1.0f, 1.0f}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 1.0f);
    return expect(pixel(canvas, 0) == std::array<int, 3>{255, 0, 0},
                  "an opaque layer pixel replaces the page") &&
           expect(pixel(canvas, 1) == std::array<int, 3>{0, 0, 255},
                  "an undrawn pixel keeps the page");
}

bool test_translucent_and_opacity() {
    Pair layer;
    layer.fill({0.0f, 0.0f, 1.0f, 1.0f}, Color{1.0f, 0.0f, 0.0f, 0.5f});
    layer.fill({1.0f, 0.0f, 1.0f, 1.0f}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 0.5f);
    // Half red at group opacity 0.5 is a quarter red over the page; opaque at 0.5 is half.
    return expect(near(pixel(canvas, 0), 64, 0, 191, 2),
                  "alpha 0.5 under opacity 0.5 is a quarter") &&
           expect(near(pixel(canvas, 1), 128, 0, 128, 2), "alpha 1 under opacity 0.5 is half");
}

// The point of isolation: two translucent layers that overlap composite as one group. Flat, the
// overlap would show both; isolated, the top one wins inside the group and the group fades once.
bool test_overlap_is_isolated() {
    Pair layer;
    layer.fill({0.0f, 0.0f, 2.0f, 1.0f}, Color{1.0f, 0.0f, 0.0f, 1.0f});
    layer.fill({1.0f, 0.0f, 2.0f, 1.0f}, Color{0.0f, 1.0f, 0.0f, 1.0f});
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 0.5f);
    return expect(near(pixel(canvas, 1), 0, 128, 128, 2),
                  "the overlap shows the top child at the group opacity, not a mix");
}

bool test_region_limits_the_composite() {
    Pair layer;
    layer.fill(kAll, Color{1.0f, 0.0f, 0.0f, 1.0f});
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, {1.0f, 0.0f, 2.0f, 1.0f}, 1.0f);
    return expect(pixel(canvas, 0) == std::array<int, 3>{0, 0, 255},
                  "left of the region untouched") &&
           expect(pixel(canvas, 1) == std::array<int, 3>{255, 0, 0},
                  "inside the region composited") &&
           expect(pixel(canvas, 3) == std::array<int, 3>{0, 0, 255},
                  "right of the region untouched");
}

bool test_nested_layer_keeps_the_pair_consistent() {
    Pair outer;
    outer.fill({0.0f, 0.0f, 4.0f, 1.0f}, Color{1.0f, 0.0f, 0.0f, 0.5f});
    Pair inner;
    inner.fill({0.0f, 0.0f, 1.0f, 1.0f}, Color{0.0f, 1.0f, 0.0f, 1.0f});
    composite_layer(outer.black, inner.black, inner.white, kAll, 0.5f);
    composite_layer(outer.white, inner.black, inner.white, kAll, 0.5f);
    CpuCanvas canvas = page();
    composite_layer(canvas, outer.black, outer.white, kAll, 1.0f);
    // x=0: the inner green at 0.5 over the outer half red, composited over the blue page.
    // The outer group alpha there is 0.5 + 0.5 * (1 - 0.5) = 0.75 and its colour 0.25 red + 0.5
    // green.
    return expect(near(pixel(canvas, 0), 64, 128, 64, 3),
                  "an inner layer composites into the outer pair") &&
           expect(near(pixel(canvas, 1), 128, 0, 128, 2), "elsewhere the outer layer is unchanged");
}

bool test_mask() {
    Pair layer;
    layer.fill(kAll, Color{1.0f, 0.0f, 0.0f, 1.0f});
    Pair mask;
    mask.fill({0.0f, 0.0f, 1.0f, 1.0f}, Color{0.0f, 0.0f, 0.0f, 1.0f});
    mask.fill({1.0f, 0.0f, 1.0f, 1.0f}, Color{0.0f, 0.0f, 0.0f, 0.5f});
    // x=2 and x=3: the mask drew nothing.
    apply_layer_mask(layer.black, layer.white, mask.black, mask.white);
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 1.0f);
    return expect(pixel(canvas, 0) == std::array<int, 3>{255, 0, 0},
                  "an opaque mask keeps the layer") &&
           expect(near(pixel(canvas, 1), 128, 0, 128, 2), "a half mask halves the layer") &&
           expect(pixel(canvas, 2) == std::array<int, 3>{0, 0, 255}, "an undrawn mask is a hole");
}

bool test_mask_composes_with_opacity() {
    Pair layer;
    layer.fill(kAll, Color{1.0f, 0.0f, 0.0f, 1.0f});
    Pair mask;
    mask.fill(kAll, Color{0.0f, 0.0f, 0.0f, 0.5f});
    apply_layer_mask(layer.black, layer.white, mask.black, mask.white);
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 0.5f);
    return expect(near(pixel(canvas, 0), 64, 0, 191, 2),
                  "a 0.5 mask under a 0.5 layer is a quarter");
}

// x0: the mask so far only, x1: both (the part half alpha), x2: the part only, x3: neither.
// A red layer is masked by the combined result over the blue page, so red is the combined alpha.
std::array<int, 4> combined_red(MaskCompositeOp op) {
    Pair below;
    below.fill({0.0f, 0.0f, 2.0f, 1.0f}, Color{0.0f, 0.0f, 0.0f, 1.0f});
    Pair part;
    part.fill({1.0f, 0.0f, 1.0f, 1.0f}, Color{0.0f, 0.0f, 0.0f, 0.5f});
    part.fill({2.0f, 0.0f, 1.0f, 1.0f}, Color{0.0f, 0.0f, 0.0f, 1.0f});
    combine_mask_part(below.black, below.white, part.black, part.white, op);
    Pair layer;
    layer.fill(kAll, Color{1.0f, 0.0f, 0.0f, 1.0f});
    apply_layer_mask(layer.black, layer.white, below.black, below.white);
    CpuCanvas canvas = page();
    composite_layer(canvas, layer.black, layer.white, kAll, 1.0f);
    return {pixel(canvas, 0)[0], pixel(canvas, 1)[0], pixel(canvas, 2)[0], pixel(canvas, 3)[0]};
}

bool test_combine_mask_part() {
    const auto near4 = [](const std::array<int, 4>& got, const std::array<int, 4>& want) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (std::abs(got[i] - want[i]) > 2) {
                return false;
            }
        }
        return true;
    };
    // The part's alpha at x1 is 0.5 (red 128) and at x2 is 1; the mask so far is 1 at x0 and x1.
    return expect(near4(combined_red(MaskCompositeOp::Subtract), {0, 0, 255, 0}),
                  "subtract: the part where nothing is below (src * (1 - dst))") &&
           expect(near4(combined_red(MaskCompositeOp::Intersect), {0, 128, 0, 0}),
                  "intersect: the part where the mask is (src * dst)") &&
           expect(near4(combined_red(MaskCompositeOp::Exclude), {255, 128, 255, 0}),
                  "exclude: src * (1 - dst) + dst * (1 - src)") &&
           expect(near4(combined_red(MaskCompositeOp::Add), {255, 255, 255, 0}),
                  "add is source-over");
}

bool test_size_mismatch_is_a_no_op() {
    Pair layer;
    layer.fill(kAll, Color{1.0f, 0.0f, 0.0f, 1.0f});
    CpuCanvas other(2, 1);
    other.clear(Color{0.0f, 0.0f, 1.0f, 1.0f});
    composite_layer(other, layer.black, layer.white, kAll, 1.0f);
    return expect(pixel(other, 0) == std::array<int, 3>{0, 0, 255},
                  "a mismatched canvas is left alone");
}

} // namespace

int main() {
    bool ok = test_opaque_and_hole();
    ok = test_translucent_and_opacity() && ok;
    ok = test_overlap_is_isolated() && ok;
    ok = test_region_limits_the_composite() && ok;
    ok = test_nested_layer_keeps_the_pair_consistent() && ok;
    ok = test_mask() && ok;
    ok = test_mask_composes_with_opacity() && ok;
    ok = test_combine_mask_part() && ok;
    ok = test_size_mismatch_is_a_no_op() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "cpu_layer: ok\n";
    return 0;
}
