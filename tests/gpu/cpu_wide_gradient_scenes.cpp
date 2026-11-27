// SPDX-License-Identifier: MIT
//
// The gradient-family CPU paint goldens: linear gradients, the `background-image` layer list
// (#614) and radial gradients (#613). Split out of `cpu_wide_golden_tests.cpp` when the merged
// scene set passed the 500-line hard limit; the scaffolding is the same shared header, so each
// case still reads as a scene.

#include "cpu_wide_scene_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;
// -------------------------------------------------------------------------------------------------
// gradients: a vertical and a horizontal linear ramp (two stops each, the vertical one with a
// middle stop) and a third gradient cut by two nested overflow clips.
// -------------------------------------------------------------------------------------------------
void build_gradients_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    Node& vertical = add_node(root, 2, {16, 16, 120, 160});
    vertical.set_paint_data(
        gradient_payload(180.0f, {LinearGradientStop{0.0f, rgba8(29, 78, 216)},
                                  LinearGradientStop{0.5f, rgba8(124, 58, 237)},
                                  LinearGradientStop{1.0f, rgba8(219, 39, 119)}}));

    Node& horizontal = add_node(root, 3, {152, 16, 148, 70});
    horizontal.set_paint_data(
        gradient_payload(90.0f, {LinearGradientStop{0.0f, rgba8(16, 185, 129)},
                                 LinearGradientStop{1.0f, rgba8(245, 158, 11)}}));

    Node& outer = add_node(root, 4, {152, 100, 148, 76});
    styles.set(4, styled({{"background-color", "#1E293B"}}));
    clip_children(styles, 4);
    Node& inner = add_node(outer, 5, {8, 8, 120, 56});
    styles.set(5, styled({{"background-color", "#334155"}}));
    clip_children(styles, 5);
    Node& clipped = add_node(inner, 6, {-10, -10, 160, 100});
    clipped.set_paint_data(
        gradient_payload(180.0f, {LinearGradientStop{0.0f, rgba8(14, 165, 233)},
                                  LinearGradientStop{1.0f, rgba8(244, 63, 94)}}));
}

bool check_gradients(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 6, "gradients: six nodes painted") &&
              expect(stats.gradients == 3, "gradients: three linear fills emitted");
    ok = expect(channel(canvas, 76, 17, 2) > channel(canvas, 76, 17, 0),
                "gradients: the vertical ramp starts blue") &&
         ok;
    ok = expect(channel(canvas, 76, 174, 0) > channel(canvas, 76, 174, 2),
                "gradients: the vertical ramp ends red") &&
         ok;
    ok = expect(channel(canvas, 160, 50, 1) > channel(canvas, 160, 50, 0),
                "gradients: the horizontal ramp starts green") &&
         ok;
    ok = expect(channel(canvas, 295, 50, 0) > channel(canvas, 295, 50, 2),
                "gradients: the horizontal ramp ends amber") &&
         ok;
    ok = expect(channel(canvas, 170, 110, 2) > channel(canvas, 170, 110, 0),
                "gradients: the clipped ramp still paints inside both clips") &&
         ok;
    ok = expect(is_pixel(canvas, 285, 120, kCard),
                "gradients: outside the inner clip the outer panel shows") &&
         ok;
    ok = expect(is_pixel(canvas, 200, 170, kCard),
                "gradients: the nested clip cuts the gradient's bottom") &&
         ok;
    ok =
        expect(is_pixel(canvas, 310, 190, kRoot), "gradients: nothing paints outside the panels") &&
        ok;
    return ok;
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
// layers (issue #614): a two-layer stack whose top layer is a translucent ramp, so the bottom layer
// shows through it, and a three-layer stack with a `none` in the middle. CSS paints the LAST layer
// first and the FIRST layer on top, so the topmost ramp must be the one the eye sees at the top of
// the box and the bottom ramp must show through where the top one is translucent.
// -------------------------------------------------------------------------------------------------
void build_layers_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    // Two layers: the top one is a translucent vertical ramp, the bottom one an opaque horizontal
    // ramp. Where the top is opaque the bottom is hidden; where it is translucent the bottom shows.
    add_node(root, 2, {16, 16, 140, 168});
    styles.set(2,
               styled({{"background-image", "linear-gradient(180deg, rgba(239, 68, 68, 0.5) 0%, "
                                            "rgba(239, 68, 68, 0.5) 100%), "
                                            "linear-gradient(90deg, #22C55E 0%, #FACC15 100%)"}}));

    // Three layers with a `none` in the middle: the `none` paints nothing, so the bottom ramp shows
    // through the translucent top one exactly as in the two-layer case.
    add_node(root, 3, {172, 16, 132, 80});
    styles.set(3,
               styled({{"background-image", "linear-gradient(180deg, rgba(56, 189, 248, 0.5) 0%, "
                                            "rgba(56, 189, 248, 0.5) 100%), none, "
                                            "linear-gradient(90deg, #F472B6 0%, #A78BFA 100%)"}}));

    // A single layer keeps the pre-#614 output: one opaque ramp, nothing behind it.
    add_node(root, 4, {172, 112, 132, 72});
    styles.set(4,
               styled({{"background-image", "linear-gradient(180deg, #1E293B 0%, #E2E8F0 100%)"}}));
}

bool check_layers(const PaintStats& stats, const CpuCanvas& canvas) {
    // Two layers on node 2, two on node 3 (the `none` emits nothing), one on node 4.
    bool ok = expect(stats.nodes == 4, "layers: four nodes painted") &&
              expect(stats.gradients == 5, "layers: five gradient fills emitted");

    // The top layer is a flat translucent red, so on its own it would be uniform. The bottom layer
    // is a horizontal green-to-amber ramp, so the proof it shows through is that the composited box
    // still varies left to right: green-dominant at the left edge, red-dominant at the right.
    ok = expect(channel(canvas, 20, 100, 1) > channel(canvas, 20, 100, 2),
                "layers: the bottom ramp shows green through the translucent top") &&
         ok;
    ok = expect(channel(canvas, 150, 100, 0) > channel(canvas, 150, 100, 1),
                "layers: the bottom ramp shows amber through the translucent top") &&
         ok;
    // The bottom ramp is horizontal, so its colour changes left to right under the flat top layer.
    ok = expect(channel(canvas, 20, 100, 2) > channel(canvas, 150, 100, 2),
                "layers: the bottom ramp runs left to right") &&
         ok;

    // The `none` layer paints nothing: the same two-layer result, blue over pink-to-violet. The
    // bottom ramp's red channel falls and its blue rises from the left edge to the right.
    ok = expect(channel(canvas, 180, 56, 0) > channel(canvas, 295, 56, 0),
                "layers: the bottom ramp shows pink through the translucent top") &&
         ok;
    ok = expect(channel(canvas, 295, 56, 2) > channel(canvas, 180, 56, 2),
                "layers: the bottom ramp runs left to right under the none layer") &&
         ok;

    // The single layer is opaque, so it is exactly its own ramp: dark at the top, light at the
    // bottom, and nothing of the page colour inside it.
    ok = expect(channel(canvas, 238, 116, 0) < channel(canvas, 238, 180, 0),
                "layers: a single opaque layer is its own ramp") &&
         ok;
    ok = expect(is_pixel(canvas, 310, 10, kRoot), "layers: nothing paints outside the boxes") && ok;
    return ok;
}

// radials (issue #613): a centred circle, an off-centre ellipse, a repeating radial ramp and a
// radial cut by an overflow clip. The CPU sink evaluates the same record the GPU shader reads
// (gradient_math.hpp), so this baseline pins the ramp the GPU path draws as well.
// -------------------------------------------------------------------------------------------------
void build_radials_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    // A centred circle: the ramp runs from the middle out to the nearest side.
    Node& circle = add_node(root, 2, {16, 16, 120, 120});
    circle.set_paint_data(radial_payload(0.5f, 0.5f, 0.5f, 0.5f,
                                         {LinearGradientStop{0.0f, rgba8(250, 204, 21)},
                                          LinearGradientStop{0.5f, rgba8(219, 39, 119)},
                                          LinearGradientStop{1.0f, rgba8(29, 78, 216)}}));

    // An off-centre ellipse: the centre sits at 25 % / 50 % and the radii differ per axis.
    Node& ellipse = add_node(root, 3, {152, 16, 148, 70});
    ellipse.set_paint_data(radial_payload(0.25f, 0.5f, 0.75f, 0.5f,
                                          {LinearGradientStop{0.0f, rgba8(16, 185, 129)},
                                           LinearGradientStop{1.0f, rgba8(245, 158, 11)}}));

    // A repeating radial ramp: the authored cycle is short, so it repeats several times.
    Node& repeating = add_node(root, 4, {152, 100, 148, 76});
    repeating.set_paint_data(radial_payload(0.5f, 0.5f, 0.5f, 0.5f,
                                            {LinearGradientStop{0.0f, rgba8(14, 165, 233)},
                                             LinearGradientStop{0.25f, rgba8(244, 63, 94)}},
                                            /*repeating=*/true));

    // A radial cut by two nested overflow clips.
    Node& outer = add_node(root, 5, {16, 148, 120, 40});
    styles.set(5, styled({{"background-color", "#1E293B"}}));
    clip_children(styles, 5);
    Node& clipped = add_node(outer, 6, {-10, -10, 160, 60});
    clipped.set_paint_data(radial_payload(0.5f, 0.5f, 0.5f, 0.5f,
                                          {LinearGradientStop{0.0f, rgba8(167, 139, 250)},
                                           LinearGradientStop{1.0f, rgba8(34, 197, 94)}}));
}

bool check_radials(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 6, "radials: six nodes painted") &&
              expect(stats.gradients == 4, "radials: four radial fills emitted");
    // The circle's centre is the first stop (amber) and its edge the last (blue).
    ok = expect(channel(canvas, 76, 76, 0) > channel(canvas, 76, 76, 2),
                "radials: the circle centre is the first stop") &&
         ok;
    ok = expect(channel(canvas, 76, 20, 2) > channel(canvas, 76, 20, 0),
                "radials: the circle edge is the last stop") &&
         ok;
    // Past the last stop the ramp holds its last colour (CSS Images 3 section 3.6), so the box
    // corner - beyond the circle's radius - is the last stop, not the page behind it. The banded
    // grid used to leave it unpainted; the per-fragment shader (ADR-076) fills the whole box.
    ok = expect(channel(canvas, 20, 20, 2) > channel(canvas, 20, 20, 0) &&
                    !is_pixel(canvas, 20, 20, kRoot),
                "radials: the box corner holds the last stop") &&
         ok;
    // The off-centre ellipse is green at its centre (25 % of the width) and amber at its far edge.
    ok = expect(channel(canvas, 152 + 37, 51, 1) > channel(canvas, 152 + 37, 51, 0),
                "radials: the ellipse centre is the first stop") &&
         ok;
    ok = expect(channel(canvas, 152 + 140, 51, 0) > channel(canvas, 152 + 140, 51, 1),
                "radials: the ellipse far edge is the last stop") &&
         ok;
    // The repeating ramp alternates: along the horizontal radius the blue-vs-red dominance must
    // flip more than once, which a single (non-repeating) ramp cannot do. The node is x 152..300,
    // y 100..176, so its centre row is y = 138 and the radius reaches x = 226 + 74.
    int flips = 0;
    int previous = 0;
    for (std::uint32_t x = 228; x < 298; x += 2) {
        const int sign = static_cast<int>(channel(canvas, x, 138, 2)) -
                         static_cast<int>(channel(canvas, x, 138, 0));
        const int current = sign > 10 ? 1 : (sign < -10 ? -1 : 0);
        if (current != 0 && previous != 0 && current != previous) {
            ++flips;
        }
        if (current != 0) {
            previous = current;
        }
    }
    ok = expect(flips >= 2, "radials: the repeating ramp alternates along the radius") && ok;
    // The clipped radial paints inside the clip and not outside it.
    ok = expect(channel(canvas, 76, 168, 2) > channel(canvas, 76, 168, 1),
                "radials: the clipped radial paints inside the clip") &&
         ok;
    ok = expect(is_pixel(canvas, 76, 190, kRoot),
                "radials: the clip cuts the radial at the panel edge") &&
         ok;
    return ok;
}
} // namespace

// -------------------------------------------------------------------------------------------------
// conics (issue #617): a centred sweep, an off-centre sweep with a start angle, and a repeating
// sweep. The tessellation is the shared one both sinks call, so this baseline pins the geometry the
// GPU path emits as well.
// -------------------------------------------------------------------------------------------------
void build_conics_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    // A centred sweep from 12 o'clock: the first stop is at the top, the last back at the top.
    Node& centred = add_node(root, 2, {16, 16, 120, 120});
    centred.set_paint_data(conic_payload(0.5f, 0.5f, 0.0f,
                                         {LinearGradientStop{0.0f, rgba8(250, 204, 21)},
                                          LinearGradientStop{0.5f, rgba8(219, 39, 119)},
                                          LinearGradientStop{1.0f, rgba8(250, 204, 21)}}));

    // An off-centre sweep that starts at 90deg, so the first stop sits at 3 o'clock.
    Node& offset = add_node(root, 3, {152, 16, 148, 70});
    offset.set_paint_data(conic_payload(0.25f, 0.5f, 90.0f,
                                        {LinearGradientStop{0.0f, rgba8(16, 185, 129)},
                                         LinearGradientStop{1.0f, rgba8(245, 158, 11)}}));

    // A repeating sweep: the authored cycle is short, so it repeats several times around the turn.
    Node& repeating = add_node(root, 4, {152, 100, 148, 76});
    repeating.set_paint_data(conic_payload(0.5f, 0.5f, 0.0f,
                                           {LinearGradientStop{0.0f, rgba8(14, 165, 233)},
                                            LinearGradientStop{0.25f, rgba8(244, 63, 94)}},
                                           /*repeating=*/true));
}

bool check_conics(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 4, "conics: four nodes painted (root plus three)") &&
              expect(stats.gradients == 3, "conics: three conic fills emitted");
    // The centred sweep's first stop is amber at 12 o'clock and the last is amber again, so the top
    // and the bottom of the box agree while the sides differ.
    ok = expect(channel(canvas, 76, 20, 0) > channel(canvas, 76, 20, 2),
                "conics: the sweep starts at 12 o'clock") &&
         ok;
    // 9 o'clock is three quarters of the turn, which is between the pink middle stop and the amber
    // last one, so it is neither the first stop nor the last.
    ok = expect(channel(canvas, 20, 76, 0) > channel(canvas, 76, 20, 0) ||
                    channel(canvas, 20, 76, 2) > channel(canvas, 76, 20, 2),
                "conics: 9 o'clock is a different point on the ramp than 12 o'clock") &&
         ok;
    // The off-centre sweep starts at 90deg, so its first stop (green) is at 3 o'clock.
    ok = expect(channel(canvas, 290, 51, 1) > channel(canvas, 290, 51, 0),
                "conics: `from 90deg` puts the first stop at 3 o'clock") &&
         ok;
    // The repeating sweep alternates, so two points a quarter turn apart differ.
    ok = expect(channel(canvas, 226, 138, 2) != channel(canvas, 226, 100, 2),
                "conics: the repeating sweep alternates around the turn") &&
         ok;
    ok = expect(is_pixel(canvas, 310, 10, kRoot), "conics: nothing paints outside the boxes") && ok;
    return ok;
}
namespace tessera::poc::batched2d::cpu_wide_support {

const std::vector<Case>& gradient_cases() {
    static const std::vector<Case> cases{
        {"gradients", build_gradients_scene, check_gradients},
        {"layers", build_layers_scene, check_layers},
        {"radials", build_radials_scene, check_radials},
        {"conics", build_conics_scene, check_conics},
    };
    return cases;
}

} // namespace tessera::poc::batched2d::cpu_wide_support
