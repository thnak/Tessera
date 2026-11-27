// SPDX-License-Identifier: MIT
//
// The rounded-corner CPU paint golden (issue #617): a gradient, a radial gradient and a solid
// background on boxes with border radii, through the real paint pass and `CpuPaintSink`. The sink
// cuts each along its arc, so the corners show the page and the GPU twin
// (tests/gpu/batched2d/gradient_gpu_tests.cpp) agrees within the blend arithmetic.

#include "cpu_wide_scene_support.hpp"

#include <vector>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;

void radius(StubStyles& styles, int id, const char* value, const char* fill_name,
            const char* fill_value) {
    styles.set(id, styled({{"border-top-left-radius", value},
                           {"border-top-right-radius", value},
                           {"border-bottom-right-radius", value},
                           {"border-bottom-left-radius", value},
                           {fill_name, fill_value}}));
}

// rounded: three boxes over the page colour.
//   2: a 140x80 vertical linear gradient with a 24px radius.
//   3: a 132x80 radial gradient with a 40px radius (a stadium: half the height).
//   4: a 140x72 solid fill with radius 16.
void build_rounded_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    add_node(root, 2, {16, 16, 140, 80});
    radius(styles, 2, "24px", "background-image", "linear-gradient(#EF4444, #3B82F6)");

    add_node(root, 3, {172, 16, 132, 80});
    radius(styles, 3, "40px", "background-image", "radial-gradient(circle, #FACC15, #DB2777)");

    add_node(root, 4, {16, 112, 140, 72});
    radius(styles, 4, "16px", "background-color", "#22C55E");
}

bool check_rounded(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 4, "rounded: four nodes painted");
    // Each box's corner pixel lies outside its arc and keeps the page colour.
    ok = expect(is_pixel(canvas, 16, 16, kRoot), "rounded: the gradient's corner is cut") && ok;
    ok = expect(is_pixel(canvas, 172, 16, kRoot), "rounded: the stadium's corner is cut") && ok;
    ok = expect(is_pixel(canvas, 16, 112, kRoot), "rounded: the solid box's corner is cut") && ok;
    // The middle of each edge is fully inside.
    ok = expect(!is_pixel(canvas, 86, 16, kRoot), "rounded: the gradient's top edge is painted") &&
         ok;
    ok = expect(!is_pixel(canvas, 172, 56, kRoot), "rounded: the stadium's left edge is painted") &&
         ok;
    ok =
        expect(!is_pixel(canvas, 86, 112, kRoot), "rounded: the solid box's top edge is painted") &&
        ok;
    return ok;
}

} // namespace

namespace tessera::poc::batched2d::cpu_wide_support {

const std::vector<Case>& rounded_cases() {
    static const std::vector<Case> cases{
        {"rounded", build_rounded_scene, check_rounded},
    };
    return cases;
}

} // namespace tessera::poc::batched2d::cpu_wide_support
