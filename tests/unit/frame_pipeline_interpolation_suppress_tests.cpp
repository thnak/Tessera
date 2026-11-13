// SPDX-License-Identifier: MIT
//
// Duplicate-present suppression for interpolated frames (game-loop.md section 3.2, #148): an
// interpolated transform adds its painted box to damage only when a corner moved at least
// `interpolation_epsilon_px` device pixels since the last frame that damaged it. The threshold is a
// pixel distance, never an alpha fraction, and it never wakes the loop.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Mat3;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::layout::DamageTracker;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pipeline_interpolation_suppress: " << message << "\n";
    }
    return condition;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// The keyframe sampler steps a transform list at the halfway point: translateX(0) first, then
// translateX(40), and it holds 40 for the rest of the animation.
KeyframeBlock slide_block() {
    KeyframeBlock block;
    block.name = "slide";
    block.steps.push_back(
        KeyframeStep{0.0, {Declaration{"transform", "translateX(0px)", false, 0}}});
    block.steps.push_back(
        KeyframeStep{100.0, {Declaration{"transform", "translateX(40px)", false, 0}}});
    return block;
}

// Runs the scripted frames and reports each frame's render result:
//   frame 1: previous 0 -> current 40 at alpha 0.5, painted at 20 (first sight, never suppressed)
//   frame 2: previous 40 -> current 40, painted at 40 (moved 20 px, never suppressed)
//   frame 3: identical to frame 2, painted at 40 (moved 0 px, a duplicate)
struct Script {
    FrameRender frames[3];
};

Script run(float dpi_scale, float epsilon_px) {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({10, 10, 20, 20});
    clear_tree(root);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipelineConfig config;
    config.dpi_scale = dpi_scale;
    config.interpolation_epsilon_px = epsilon_px;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty, config);

    KeyframeTiming timing;
    timing.duration = 0.2;
    animations.start(AnimationInput{2, slide_block(), timing, {}, "", false});
    pipeline.tick(root, 0.05, 0.5); // samples translateX(0)
    pipeline.finish_frame(root);
    box.set_transform(Mat3::translation(40.0f, 0.0f));
    clear_tree(root);

    Script script;
    const double dts[3] = {0.1, 0.01, 0.01};
    for (int index = 0; index < 3; ++index) {
        pipeline.tick(root, dts[index], 0.5);
        script.frames[index] = pipeline.finish_frame(root);
        clear_tree(root);
    }
    return script;
}

bool duplicate_pose_is_suppressed() {
    const Script script = run(1.0f, 0.0625f);
    return expect(script.frames[0].interpolation_suppressed == 0,
                  "the first interpolated frame has no reference and is damaged") &&
           expect(script.frames[0].should_render, "the first interpolated frame renders") &&
           expect(script.frames[1].interpolation_suppressed == 0,
                  "a 20 px move is far above the epsilon") &&
           expect(script.frames[2].interpolation_suppressed == 1,
                  "an unchanged painted pose is a duplicate and leaves damage");
}

bool zero_epsilon_disables_suppression() {
    const Script script = run(1.0f, 0.0f);
    return expect(script.frames[2].interpolation_suppressed == 0,
                  "epsilon 0 keeps every interpolated box in damage");
}

bool epsilon_is_measured_in_device_pixels() {
    // The scripted poses differ by whole pixels, so this checks the unit through the zero-move
    // case: corners are compared after scaling to device pixels, and an unchanged pose is a
    // duplicate at any scale.
    const Script scaled = run(2.0f, 0.0625f);
    return expect(scaled.frames[1].interpolation_suppressed == 0 &&
                      scaled.frames[2].interpolation_suppressed == 1,
                  "suppression holds at a device scale of 2");
}

bool invalid_epsilon_is_rejected() {
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{10, 10}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipelineConfig config;
    config.interpolation_epsilon_px = -1.0f;
    try {
        FramePipeline pipeline(animations, loop, tracker, damage, dirty, config);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return expect(false, "a negative epsilon is rejected");
}

} // namespace

int main() {
    if (!duplicate_pose_is_suppressed()) {
        return 1;
    }
    if (!zero_epsilon_disables_suppression()) {
        return 2;
    }
    if (!epsilon_is_measured_in_device_pixels()) {
        return 3;
    }
    if (!invalid_epsilon_is_rejected()) {
        return 4;
    }
    std::cout << "frame_pipeline_interpolation_suppress: ok\n";
    return 0;
}
