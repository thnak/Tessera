// SPDX-License-Identifier: MIT
//
// Real-Vulkan `clip-path: polygon()` readback (ADR-068 slice 5, issue #673). Each scene paints one
// full-target quad under a shaped clip and compares every pixel with the CPU reference. The
// fixtures keep every edge crossing at least a thousandth of a pixel from a pixel centre, so the
// GPU and the CPU can only disagree when the shader's coverage rule is wrong, not when they round
// differently.
//
// Deterministic: UNORM target, AA disabled (golden config), exact byte comparison.

#include "clip_shape_scene.hpp"

#include <string>
#include <vector>

namespace {

using tessera::test::clipshape::expect;
using tessera::test::clipshape::Fixture;
using tessera::test::clipshape::Outcome;
using tessera::test::clipshape::Scene;
namespace clip = tessera::rendering::gpu::clip;

std::string zigzag(std::uint32_t count) {
    std::string css = "polygon(";
    for (std::uint32_t index = 0; index < count; ++index) {
        css += index == 0 ? "" : ", ";
        css += std::to_string(index % 2 == 0 ? 8 : 56) + "px " +
               std::to_string(index % 4 < 2 ? 8 : 56) + "px";
    }
    return css + ")";
}

const char* const kTriangle = "polygon(8.25px 8.25px, 56.25px 8.25px, 32.25px 56.25px)";
const char* const kLShape =
    "polygon(10.25px 10.25px, 40.25px 10.25px, 40.25px 30.25px, 25.25px 30.25px, "
    "25.25px 50.25px, 10.25px 50.25px)";
const char* const kStarNonzero =
    "polygon(nonzero, 32.1px 4.1px, 48.56px 54.75px, 5.47px 23.45px, 58.73px 23.45px, "
    "15.64px 54.75px)";
const char* const kStarEvenOdd =
    "polygon(evenodd, 32.1px 4.1px, 48.56px 54.75px, 5.47px 23.45px, 58.73px 23.45px, "
    "15.64px 54.75px)";

} // namespace

int main() {
    const auto fixture = Fixture::create("polygon_clip");
    if (fixture == nullptr) {
        return 0;
    }

    const std::vector<Scene> scenes = {
        {"convex triangle", {kTriangle}, false, 0},
        {"concave L", {kLShape}, false, 0},
        {"self-intersecting star, nonzero", {kStarNonzero}, false, 0},
        {"self-intersecting star, evenodd", {kStarEvenOdd}, false, 0},
        {"polygon under a rounded clip", {kStarNonzero}, true, 0},
        {"over the vertex limit", {zigzag(clip::kMaxPolygonVertices + 1)}, false, 1},
    };

    bool ok = true;
    std::vector<std::size_t> painted;
    for (const Scene& scene : scenes) {
        const Outcome outcome = fixture->run(scene);
        ok =
            expect(outcome.mismatches == 0, scene.name + ": " + std::to_string(outcome.mismatches) +
                                                " pixel(s) differ from the CPU reference") &&
            ok;
        ok = expect(outcome.painted > 0, scene.name + ": the reference paints something") && ok;
        ok = expect(outcome.fallbacks == scene.expected_fallbacks,
                    scene.name + ": the fallback count is " +
                        std::to_string(scene.expected_fallbacks)) &&
             ok;
        painted.push_back(outcome.painted);
    }
    // The two fill rules must differ on the star, or the test would pass with one rule ignored.
    ok = expect(painted[2] > painted[3], "nonzero paints more of the star than evenodd") && ok;
    // The over-limit polygon is its bounding box: 48 x 48 pixels.
    ok = expect(painted[5] == 48U * 48U, "an over-limit polygon paints its bounding box") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "polygon_clip: ok\n";
    return 0;
}
