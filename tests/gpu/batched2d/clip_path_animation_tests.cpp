// SPDX-License-Identifier: MIT
//
// An animated `clip-path` on the real renderer (ADR-068 slice 6, issue #674): a polygon morph is
// sampled from keyframes at fixed steps (no clock), each frame's value goes through the paint-time
// parser and `Batched2DRenderer::push_clip_shape`, and every pixel of every frame is compared with
// the CPU reference. The paint pass does not read `clip-path` yet, so this stands in for the
// golden a stylesheet-driven animation will get once it does.
//
// The morph is a square turning into a diamond. The vertices were chosen (by searching offsets,
// then replaying the sampler's own rounding) so that at each of the five sampled steps every edge
// crosses every pixel row at least a hundredth of a pixel from a pixel centre. A nicer-looking set
// with round offsets put an edge exactly through a centre at 0.25 and 0.5, where the GPU and the
// CPU round the tie differently; that is a property of the fixture, not of the shader.

#include "clip_shape_scene.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/style/keyframes.hpp"

#include <string>
#include <vector>

int main() {
    namespace animation = tessera::ui::animation;
    namespace style = tessera::ui::style;
    using tessera::test::clipshape::expect;
    using tessera::test::clipshape::Scene;

    const auto fixture = tessera::test::clipshape::Fixture::create("clip_path_animation");
    if (fixture == nullptr) {
        return 0;
    }

    style::KeyframeBlock block;
    block.name = "morph";
    block.steps.push_back(style::KeyframeStep{
        0.0,
        {style::Declaration{
            "clip-path", "polygon(8.52px 8.61px, 56.59px 8.34px, 56.52px 56.61px, 8.59px 56.34px)",
            false, 0}}});
    block.steps.push_back(style::KeyframeStep{
        100.0,
        {style::Declaration{
            "clip-path", "polygon(32.8px 4.72px, 60.24px 32.19px, 32.72px 60.8px, 4.19px 32.24px)",
            false, 0}}});
    animation::KeyframeTiming timing;
    timing.duration = 1.0;
    timing.fill = animation::FillMode::Both;

    bool ok = true;
    std::vector<std::size_t> painted;
    for (const double seconds : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const auto sampled = animation::sample_keyframes(block, timing, seconds);
        ok = expect(!sampled.empty() && sampled.front().property == "clip-path",
                    "the keyframes sample a clip-path at " + std::to_string(seconds)) &&
             ok;
        if (sampled.empty()) {
            continue;
        }
        const Scene frame{"morph at " + std::to_string(seconds), {sampled.front().value}, false, 0};
        const auto outcome = fixture->run(frame);
        ok = expect(outcome.mismatches == 0, frame.name + " (" + sampled.front().value +
                                                 "): " + std::to_string(outcome.mismatches) +
                                                 " pixel(s) differ from the CPU reference") &&
             ok;
        ok = expect(outcome.painted > 0, frame.name + ": the reference paints something") && ok;
        painted.push_back(outcome.painted);
    }
    // The shape really changes between the ends: the square covers more than the diamond, and the
    // frames in between lie between them.
    ok = expect(painted.size() == 5 && painted.front() > painted[2] && painted[2] > painted.back(),
                "the morph shrinks from the square to the diamond") &&
         ok;

    if (!ok) {
        return 1;
    }
    std::cout << "clip_path_animation: ok\n";
    return 0;
}
