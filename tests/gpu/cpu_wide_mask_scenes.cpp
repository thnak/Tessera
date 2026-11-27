// SPDX-License-Identifier: MIT
//
// The `mask-image` CPU paint golden (issue #617): the CPU twin of the GPU layer mask path
// (offscreen-layers.md sections 5.2 and 5a), through the real paint pass and `CpuPaintSink`. The
// same scene is rendered on lavapipe by `layer_mask_golden_tests`, and the two are compared there.

#include "cpu_wide_scene_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;

// -------------------------------------------------------------------------------------------------
// masks: four nodes over the page colour.
//   2: a green box under a horizontal alpha ramp, transparent on the left, opaque on the right.
//   3: a pink box at opacity 0.5 under a radial mask, opaque in the middle and clear at the edge.
//   4: a group of two overlapping opaque children under a uniform 0.5 mask. The mask scales the
//      group as a unit, so the overlap shows the top child at 0.5 and never a mix of both.
//   5: a blue box under an opaque mask, which must be indistinguishable from no mask.
// -------------------------------------------------------------------------------------------------
void build_masks_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    add_node(root, 2, {16, 16, 140, 80});
    styles.set(2, styled({{"background-color", "#22C55E"},
                          {"mask-image", "linear-gradient(90deg, rgba(0, 0, 0, 0), #000)"}}));

    add_node(root, 3, {172, 16, 132, 80});
    styles.set(3, styled({{"background-color", "#F472B6"},
                          {"opacity", "0.5"},
                          {"mask-image", "radial-gradient(ellipse, #000, rgba(0, 0, 0, 0))"}}));

    Node& group = add_node(root, 4, {16, 112, 140, 72});
    styles.set(4,
               styled({{"mask-image", "linear-gradient(rgba(0, 0, 0, 0.5), rgba(0, 0, 0, 0.5))"}}));
    add_node(group, 6, {0, 0, 90, 50});
    styles.set(6, styled({{"background-color", "#EF4444"}}));
    add_node(group, 7, {50, 22, 90, 50});
    styles.set(7, styled({{"background-color", "#3B82F6"}}));

    add_node(root, 5, {172, 112, 132, 72});
    styles.set(5, styled({{"background-color", "#3B82F6"},
                          {"mask-image", "linear-gradient(#000, #000)"}}));
}

bool near(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, int r, int g, int b,
          int tolerance, const char* what) {
    const Pixel actual = pixel(canvas, x, y);
    if (std::abs(actual[0] - r) <= tolerance && std::abs(actual[1] - g) <= tolerance &&
        std::abs(actual[2] - b) <= tolerance) {
        return true;
    }
    std::cerr << "cpu-wide-golden: masks: " << what << " at (" << x << "," << y << "): got ("
              << int{actual[0]} << "," << int{actual[1]} << "," << int{actual[2]} << "), expected ("
              << r << "," << g << "," << b << ")\n";
    return false;
}

bool check_masks(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 7, "masks: seven nodes painted");
    // 2: the ramp fades the green in from the page colour; the middle is a mix of the two.
    ok = near(canvas, 17, 56, 11, 17, 32, 6, "the left edge of the ramp is the page") && ok;
    ok = expect(channel(canvas, 60, 56, 1) > channel(canvas, 30, 56, 1) + 20 &&
                    channel(canvas, 120, 56, 1) > channel(canvas, 60, 56, 1) + 20,
                "masks: the green grows along the ramp") &&
         ok;
    ok = near(canvas, 154, 56, 34, 197, 94, 12, "the right edge of the ramp is the green") && ok;
    // 3: opacity 0.5 under an opaque-centre mask: half pink over the page; the corner is clear.
    ok = near(canvas, 238, 56, 128, 66, 107, 8, "the radial centre is half pink") && ok;
    ok = near(canvas, 173, 17, 11, 17, 32, 6, "the radial corner is the page") && ok;
    // 4: the group is scaled as a unit: one child over the page at 0.5, never a mix of the two.
    ok = near(canvas, 40, 130, 125, 43, 50, 3, "the top-left child at half") && ok;
    ok = near(canvas, 100, 150, 35, 74, 139, 3, "the overlap shows the top child at half") && ok;
    ok = near(canvas, 140, 170, 35, 74, 139, 3, "the lower child at half") && ok;
    // 5: an opaque mask changes nothing.
    ok = near(canvas, 238, 148, 59, 130, 246, 1, "an opaque mask leaves the box whole") && ok;
    ok = expect(is_pixel(canvas, 310, 190, kRoot), "masks: nothing paints outside the boxes") && ok;
    return ok;
}

} // namespace

namespace tessera::poc::batched2d::cpu_wide_support {

const std::vector<Case>& mask_cases() {
    static const std::vector<Case> cases{
        {"masks", build_masks_scene, check_masks},
    };
    return cases;
}

} // namespace tessera::poc::batched2d::cpu_wide_support
