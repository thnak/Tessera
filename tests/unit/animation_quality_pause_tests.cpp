// SPDX-License-Identifier: MIT
//
// Adaptive quality's last ladder position (#147): non-essential animations (every animated property
// is paint-only and neither `transform` nor `opacity`) freeze at their current value and stop
// keeping the loop awake; essential and mixed animations keep running; recovery resumes the frozen
// ones where they stopped and never resumes one the app paused itself.

#include "tessera/ui/animation/runtime.hpp"

#include <cstddef>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_quality_pause: " << message << "\n";
    }
    return condition;
}

KeyframeBlock block_of(const char* name, std::initializer_list<const char*> properties) {
    KeyframeBlock block;
    block.name = name;
    KeyframeStep from{0.0, {}};
    KeyframeStep to{100.0, {}};
    for (const char* property : properties) {
        from.declarations.push_back(Declaration{property, "0", false, 0});
        to.declarations.push_back(Declaration{property, "1", false, 0});
    }
    block.steps.push_back(from);
    block.steps.push_back(to);
    return block;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, const char* name,
                  std::initializer_list<const char*> properties, bool paused = false) {
    KeyframeTiming timing;
    timing.duration = 10.0;
    return runtime.start(
        AnimationInput{target, block_of(name, properties), timing, {}, "", paused});
}

double elapsed_of(const AnimationRuntime& runtime, AnimationId id) {
    for (const auto& entry : runtime.entries()) {
        if (entry.id == id) {
            return entry.elapsed;
        }
    }
    return -1.0;
}

bool classification_follows_the_property_rule() {
    AnimationRuntime runtime;
    const AnimationId color = start(runtime, 1, "color", {"background-color"});
    const AnimationId fade = start(runtime, 2, "fade", {"opacity"});
    const AnimationId slide = start(runtime, 3, "slide", {"transform"});
    const AnimationId grow = start(runtime, 4, "grow", {"width"});
    const AnimationId mixed = start(runtime, 5, "mixed", {"background-color", "opacity"});
    const AnimationId empty = start(runtime, 6, "empty", {});
    bool ok = true;
    for (const auto& entry : runtime.entries()) {
        const bool expected = entry.id == color;
        ok = expect(
                 entry.non_essential == expected,
                 "only a pure paint-only, non-transform, non-opacity animation is non-essential") &&
             ok;
    }
    (void)fade;
    (void)slide;
    (void)grow;
    (void)mixed;
    (void)empty;
    return ok;
}

bool non_essential_freezes_and_resumes_where_it_stopped() {
    AnimationRuntime runtime;
    const AnimationId color = start(runtime, 1, "color", {"background-color"});
    const AnimationId slide = start(runtime, 2, "slide", {"transform"});
    (void)runtime.tick(1.0);
    bool ok = expect(elapsed_of(runtime, color) == 1.0 && elapsed_of(runtime, slide) == 1.0,
                     "both advance before the suspension");

    runtime.set_suspend_non_essential(true);
    ok = expect(runtime.suspended() == 1, "exactly the colour animation is frozen") && ok;
    const auto frozen_tick = runtime.tick(1.0);
    ok = expect(elapsed_of(runtime, color) == 1.0, "the frozen animation keeps its elapsed time") &&
         ok;
    ok = expect(elapsed_of(runtime, slide) == 2.0, "the transform animation keeps running") && ok;
    ok = expect(frozen_tick.keep_awake,
                "a running essential animation still keeps the loop awake") &&
         ok;

    runtime.set_suspend_non_essential(false);
    (void)runtime.tick(1.0);
    ok = expect(elapsed_of(runtime, color) == 2.0,
                "it resumes from where it stopped, not restarted") &&
         ok;
    return ok;
}

bool frozen_only_scene_does_not_keep_the_loop_awake() {
    AnimationRuntime runtime;
    (void)start(runtime, 1, "color", {"background-color"});
    runtime.set_suspend_non_essential(true);
    const auto tick = runtime.tick(1.0);
    bool ok = expect(!tick.keep_awake, "a scene with only frozen animations lets the loop idle");
    const auto bad_dt = runtime.tick(-1.0);
    ok = expect(!bad_dt.keep_awake, "an invalid dt does not wake it either") && ok;
    runtime.set_suspend_non_essential(false);
    ok = expect(runtime.tick(1.0).keep_awake, "resuming makes the animation keep the loop awake") &&
         ok;
    return ok;
}

bool app_pause_is_independent_of_suspension() {
    AnimationRuntime runtime;
    const AnimationId color = start(runtime, 1, "color", {"background-color"}, true);
    runtime.set_suspend_non_essential(true);
    bool ok =
        expect(runtime.suspended() == 0, "an app-paused animation is not counted as suspended");
    runtime.set_suspend_non_essential(false);
    (void)runtime.tick(1.0);
    ok = expect(elapsed_of(runtime, color) == 0.0,
                "recovery does not resume an animation the app paused itself") &&
         ok;
    return ok;
}

bool animation_started_while_suspended_is_frozen() {
    AnimationRuntime runtime;
    runtime.set_suspend_non_essential(true);
    const AnimationId color = start(runtime, 1, "color", {"background-color"});
    (void)runtime.tick(1.0);
    return expect(elapsed_of(runtime, color) == 0.0, "a new non-essential animation starts frozen");
}

} // namespace

int main() {
    if (!classification_follows_the_property_rule()) {
        return 1;
    }
    if (!non_essential_freezes_and_resumes_where_it_stopped()) {
        return 2;
    }
    if (!frozen_only_scene_does_not_keep_the_loop_awake()) {
        return 3;
    }
    if (!app_pause_is_independent_of_suspension()) {
        return 4;
    }
    if (!animation_started_while_suspended_is_frozen()) {
        return 5;
    }
    std::cout << "animation_quality_pause: ok\n";
    return 0;
}
