// SPDX-License-Identifier: MIT
//
// The colour-stop-position CPU paint golden (issue #617, CSS Images 4 section 3.5): the natural
// stylesheet forms - a stop with two positions, px stop positions on a repeating linear and radial
// ramp, and conic angles - through the real paint pass and `CpuPaintSink`. Chromium measurements of
// the same values are in tests/gpu/batched2d/gradient_chromium_reference_stops.hpp.

#include "cpu_wide_scene_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;

// -------------------------------------------------------------------------------------------------
// stops: four nodes over the page colour.
//   2: hard stripes, `repeating-linear-gradient(90deg, red 0 10px, blue 10px 20px)`: seven periods
//      across 140px, each stop pair a px length.
//   3: `linear-gradient(90deg, green 0 25%, yellow 25% 50%, pink 50% 100%)`: three flat bands,
//   every
//      colour written once with two positions.
//   4: `repeating-radial-gradient(circle, yellow 0, pink 12px)`: a 12px px period along the ray,
//      repeating out into the corners (the box is 140x72, so the ray is 70px, 36px to the edge).
//   5: `conic-gradient(from 20deg, red 0deg 90deg, green 90deg 180deg, blue 270deg)`: two flat
//      quarter turns and a ramp back to the first colour.
// -------------------------------------------------------------------------------------------------
void build_stops_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    add_node(root, 2, {16, 16, 140, 80});
    styles.set(2,
               styled({{"background-image",
                        "repeating-linear-gradient(90deg, #EF4444 0 10px, #3B82F6 10px 20px)"}}));

    add_node(root, 3, {172, 16, 132, 80});
    styles.set(
        3, styled({{"background-image",
                    "linear-gradient(90deg, #22C55E 0 25%, #FACC15 25% 50%, #F472B6 50% 100%)"}}));

    add_node(root, 4, {16, 112, 140, 72});
    styles.set(4, styled({{"background-image",
                           "repeating-radial-gradient(circle, #FACC15 0, #DB2777 12px)"}}));

    add_node(root, 5, {172, 112, 132, 72});
    styles.set(5, styled({{"background-image",
                           "conic-gradient(from 20deg, #EF4444 0deg 90deg, #22C55E 90deg 180deg, "
                           "#3B82F6 270deg)"}}));
}

bool near(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y, int r, int g, int b,
          int tolerance, const char* what) {
    const Pixel actual = pixel(canvas, x, y);
    if (std::abs(actual[0] - r) <= tolerance && std::abs(actual[1] - g) <= tolerance &&
        std::abs(actual[2] - b) <= tolerance) {
        return true;
    }
    std::cerr << "cpu-wide-golden: stops: " << what << " at (" << x << "," << y << "): got ("
              << int{actual[0]} << "," << int{actual[1]} << "," << int{actual[2]} << "), expected ("
              << r << "," << g << "," << b << ")\n";
    return false;
}

bool check_stops(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 5, "stops: five nodes painted") &&
              expect(stats.gradients == 4, "stops: four gradient fills emitted");
    // 2: x 16..26 red, 26..36 blue, 36..46 red again (a 20px period).
    ok = near(canvas, 20, 56, 239, 68, 68, 1, "the first stripe is red") && ok;
    ok = near(canvas, 30, 56, 59, 130, 246, 1, "the second stripe is blue") && ok;
    ok = near(canvas, 40, 56, 239, 68, 68, 1, "the pattern repeats") && ok;
    ok = near(canvas, 150, 56, 59, 130, 246, 1, "the seventh period still repeats") && ok;
    // 3: 25% of 132px is 33px: green to x=205, yellow to x=238, pink after.
    ok = near(canvas, 190, 56, 34, 197, 94, 1, "the first band is green") && ok;
    ok = near(canvas, 220, 56, 250, 204, 21, 1, "the second band is yellow") && ok;
    ok = near(canvas, 290, 56, 244, 114, 182, 1, "the third band is pink") && ok;
    // 4: the ray starts yellow at the centre (86, 148) and has fully moved to pink 12px out.
    ok = near(canvas, 86, 148, 250, 204, 21, 12, "the radial centre is yellow") && ok;
    ok = near(canvas, 86 + 11, 148, 219, 39, 119, 24, "just before the period ends it is pink") &&
         ok;
    ok = near(canvas, 86 + 12, 148, 250, 204, 21, 24, "and the period restarts at yellow") && ok;
    // 5: the sweep is flat red from the start angle, then flat green; the box centre is (238, 148).
    ok = near(canvas, 260, 125, 239, 68, 68, 1, "the first quarter turn is red") && ok;
    ok = near(canvas, 260, 170, 34, 197, 94, 1, "the second quarter turn is green") && ok;
    ok = expect(is_pixel(canvas, 310, 190, kRoot), "stops: nothing paints outside the boxes") && ok;
    return ok;
}

} // namespace

namespace tessera::poc::batched2d::cpu_wide_support {

const std::vector<Case>& stop_cases() {
    static const std::vector<Case> cases{
        {"stops", build_stops_scene, check_stops},
    };
    return cases;
}

} // namespace tessera::poc::batched2d::cpu_wide_support
