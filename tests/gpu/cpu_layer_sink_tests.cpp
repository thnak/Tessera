// SPDX-License-Identifier: MIT
//
// `CpuPaintSink` isolated layers and layer masks (issue #617), the CPU twin of the GPU path: the
// sink calls themselves (composite once, clip, nesting, the mask's refusals) and the real paint
// pass (group opacity isolation, `mask-image`). Vulkan-free; exact bytes up to a tolerance of 2
// where a float composite is involved.

#include "cpu_wide_scene_support.hpp"

#include <cstdlib>
#include <iostream>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;
using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::paint::LayerDraw;
using tessera::ui::paint::PaintContext;

constexpr Rect kFrame{0.0f, 0.0f, 8.0f, 2.0f};
constexpr Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};

CpuCanvas blue_page() {
    CpuCanvas canvas(8, 2);
    canvas.clear(Color{0.0f, 0.0f, 1.0f, 1.0f});
    return canvas;
}

bool near(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, int r, int g, int b,
          const char* what) {
    const Pixel actual = pixel(canvas, x, y);
    if (std::abs(actual[0] - r) <= 2 && std::abs(actual[1] - g) <= 2 &&
        std::abs(actual[2] - b) <= 2) {
        return true;
    }
    std::cerr << "cpu_layer_sink: " << what << " at (" << x << "," << y << "): got ("
              << int{actual[0]} << "," << int{actual[1]} << "," << int{actual[2]} << "), expected ("
              << r << "," << g << "," << b << ")\n";
    return false;
}

bool layer_composites_once() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    bool ok = expect(sink.supports_layers(), "the CPU sink supports layers");
    sink.push_layer(LayerDraw{kFrame, 0.5f});
    sink.fill_rect({0, 0, 4, 2}, kRed);
    sink.fill_rect({2, 0, 4, 2}, kGreen);
    sink.pop_layer();
    ok = near(canvas, 0, 0, 128, 0, 128, "red alone at 0.5") && ok;
    ok = near(canvas, 3, 0, 0, 128, 128, "the overlap shows the top draw at 0.5, not a mix") && ok;
    ok = near(canvas, 7, 0, 0, 0, 255, "outside the draws is the page") && ok;
    return ok;
}

bool layer_respects_box_and_clip() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.push_layer(LayerDraw{{1, 0, 5, 2}, 1.0f});
    sink.fill_rect({0, 0, 8, 2}, kRed);
    sink.pop_layer();
    bool ok = near(canvas, 0, 0, 0, 0, 255, "left of the layer box is the page");
    ok = near(canvas, 3, 0, 255, 0, 0, "inside the layer box is the layer") && ok;
    ok = near(canvas, 6, 0, 0, 0, 255, "right of the layer box is the page") && ok;

    CpuCanvas clipped = blue_page();
    CpuPaintSink clipped_sink(clipped);
    clipped_sink.push_layer(LayerDraw{kFrame, 1.0f});
    clipped_sink.fill_rect({0, 0, 8, 2}, kRed);
    clipped_sink.push_clip({{2, 0, 2, 2}});
    clipped_sink.pop_clip();
    clipped_sink.pop_layer();
    ok = near(clipped, 0, 0, 255, 0, 0, "a balanced clip leaves the layer whole") && ok;
    return ok;
}

bool unbalanced_pop_is_ignored() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.pop_layer();
    sink.end_layer_mask();
    return near(canvas, 0, 0, 0, 0, 255, "pops with no open layer change nothing");
}

bool mask_refusals() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    bool ok = expect(!sink.begin_layer_mask(), "no open layer: the mask is refused");
    sink.push_layer(LayerDraw{kFrame, 1.0f});
    sink.fill_rect(kFrame, kRed);
    ok = expect(sink.begin_layer_mask(), "an open layer takes a mask") && ok;
    sink.end_layer_mask();
    ok = expect(!sink.begin_layer_mask(), "a masked layer takes no second mask") && ok;
    sink.pop_layer();
    // The mask drew nothing, so the whole layer is a hole.
    ok = near(canvas, 4, 0, 0, 0, 255, "an undrawn mask leaves the page") && ok;
    return ok;
}

bool mask_scales_the_layer() {
    CpuCanvas canvas = blue_page();
    CpuPaintSink sink(canvas);
    sink.push_layer(LayerDraw{kFrame, 1.0f});
    sink.fill_rect(kFrame, kRed);
    if (!sink.begin_layer_mask()) {
        return expect(false, "the mask was refused");
    }
    sink.fill_rect({0, 0, 2, 2}, Color{0, 0, 0, 1.0f});
    sink.fill_rect({2, 0, 2, 2}, Color{0, 0, 0, 0.5f});
    sink.end_layer_mask();
    sink.pop_layer();
    bool ok = near(canvas, 0, 0, 255, 0, 0, "opaque mask keeps the layer");
    ok = near(canvas, 2, 0, 128, 0, 128, "half mask halves the layer") && ok;
    ok = near(canvas, 5, 1, 0, 0, 255, "outside the mask is a hole") && ok;
    // Draws after the mask go to the layer's parent again.
    sink.fill_rect({6, 0, 2, 2}, kGreen);
    return near(canvas, 6, 0, 0, 255, 0, "the frame canvas takes draws after the pop") && ok;
}

// Through the real paint pass: a faded group isolates only when the pass is asked to.
bool group_opacity_through_the_pass() {
    Node root(1);
    StubStyles styles;
    root.set_bounds(kFrame);
    Node& group = add_node(root, 2, {0, 0, 8, 2});
    styles.set(2, styled({{"opacity", "0.5"}}));
    add_node(group, 3, {0, 0, 4, 2});
    styles.set(3, styled({{"background-color", "#FF0000"}}));
    add_node(group, 4, {2, 0, 4, 2});
    styles.set(4, styled({{"background-color", "#00FF00"}}));

    CpuCanvas flat = blue_page();
    CpuPaintSink flat_sink(flat);
    tessera::ui::paint::paint_scene(root, styles, flat_sink);
    CpuCanvas isolated = blue_page();
    CpuPaintSink isolated_sink(isolated);
    PaintContext context;
    context.isolate_group_opacity = true;
    tessera::ui::paint::paint_scene(root, styles, isolated_sink, context);
    // Flat, each primitive carries the 0.5: the green over (red at 0.5 over blue) leaves red in the
    // mix.
    bool ok = expect(channel(flat, 3, 0, 0) > 30, "flat: the overlap still shows the red below");
    ok = near(isolated, 3, 0, 0, 128, 128, "isolated: the overlap is the top child at 0.5") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = layer_composites_once();
    ok = layer_respects_box_and_clip() && ok;
    ok = unbalanced_pop_is_ignored() && ok;
    ok = mask_refusals() && ok;
    ok = mask_scales_the_layer() && ok;
    ok = group_opacity_through_the_pass() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "cpu_layer_sink: ok\n";
    return 0;
}
