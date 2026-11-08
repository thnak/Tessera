// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/spring_runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::Direction;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::SpringAnimationInput;
using tessera::ui::animation::SpringRuntime;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_reduced_motion: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 0.001;
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

KeyframeBlock mixed_block() {
    KeyframeBlock block = opacity_block();
    block.name = "mixed";
    block.steps[0].declarations.push_back(Declaration{"width", "10px", false, 0});
    block.steps[1].declarations.push_back(Declaration{"width", "20px", false, 0});
    return block;
}

KeyframeBlock width_block() {
    KeyframeBlock block;
    block.name = "grow";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"width", "10px", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"width", "20px", false, 0}}});
    return block;
}

AnimationId start(AnimationRuntime& runtime, KeyframeBlock keyframes, double duration,
                  double delay = 0.0, double iterations = 1.0,
                  Direction direction = Direction::Normal, FillMode fill = FillMode::None) {
    AnimationInput input;
    input.target = 1;
    input.keyframes = std::move(keyframes);
    input.timing.duration = duration;
    input.timing.delay = delay;
    input.timing.iterations = iterations;
    input.timing.direction = direction;
    input.timing.fill = fill;
    return runtime.start(std::move(input));
}

const tessera::ui::animation::AnimationSample* sample_for(const AnimationTickResult& result,
                                                          AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id == id) {
            return &sample;
        }
    }
    return nullptr;
}

const std::string* value_for(const tessera::ui::animation::AnimationSample* sample,
                             const std::string& property) {
    if (sample == nullptr) {
        return nullptr;
    }
    for (const auto& declaration : sample->declarations) {
        if (declaration.property == property) {
            return &declaration.value;
        }
    }
    return nullptr;
}

bool finished_has(const AnimationTickResult& result, AnimationId id) {
    for (const AnimationId finished : result.finished) {
        if (finished == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    AnimationRuntime full;
    if (!expect(!full.reduced_motion(), "full default")) {
        return 1;
    }
    const AnimationId full_id =
        start(full, opacity_block(), 2.0, 1.0, 3.0, Direction::Alternate, FillMode::Forwards);
    if (!expect(full.entries().front().id == full_id, "full id") ||
        !expect(near(full.entries().front().timing.duration, 2.0), "full duration") ||
        !expect(near(full.entries().front().timing.delay, 1.0), "full delay") ||
        !expect(full.entries().front().timing.direction == Direction::Alternate,
                "full direction")) {
        return 2;
    }

    AnimationRuntime reduced;
    reduced.set_reduced_motion(true);
    AnimationId opacity_id = 0;
    opacity_id =
        start(reduced, opacity_block(), 2.0, 1.0, 3.0, Direction::Alternate, FillMode::Forwards);
    if (!expect(reduced.reduced_motion(), "reduced enabled") ||
        !expect(reduced.entries().front().timing.duration == 0.12, "opacity clamped") ||
        !expect(reduced.entries().front().timing.delay == 0.0, "opacity delay removed") ||
        !expect(reduced.entries().front().timing.iterations == 1.0, "opacity single run") ||
        !expect(reduced.entries().front().timing.direction == Direction::Normal,
                "opacity direction normal") ||
        !expect(reduced.entries().front().timing.fill == FillMode::Forwards, "fill kept")) {
        return 3;
    }
    const auto opacity_tick = reduced.tick(0.2);
    if (!expect(finished_has(opacity_tick, opacity_id), "opacity finishes") ||
        !expect(reduced.settled() == 1, "opacity fill retained")) {
        return 4;
    }

    AnimationRuntime mixed;
    mixed.set_reduced_motion(true);
    AnimationId mixed_id =
        start(mixed, mixed_block(), 2.0, 1.0, 3.0, Direction::Alternate, FillMode::Forwards);
    if (!expect(mixed.entries().front().timing.duration == 0.0, "mixed instant") ||
        !expect(mixed.entries().front().timing.delay == 0.0, "mixed no delay")) {
        return 5;
    }
    const auto mixed_tick = mixed.tick(0.01);
    const auto* mixed_sample = sample_for(mixed_tick, mixed_id);
    const std::string* mixed_opacity = value_for(mixed_sample, "opacity");
    const std::string* mixed_width = value_for(mixed_sample, "width");
    if (!expect(finished_has(mixed_tick, mixed_id), "mixed finishes immediately") ||
        !expect(mixed_opacity != nullptr && near(std::stof(*mixed_opacity), 1.0),
                "mixed final opacity") ||
        !expect(mixed_width != nullptr && mixed_width->starts_with("20"), "mixed final width")) {
        return 6;
    }

    AnimationRuntime width_only;
    width_only.set_reduced_motion(true);
    AnimationId width_id = start(width_only, width_block(), 2.0);
    const auto width_tick = width_only.tick(0.01);
    if (!expect(finished_has(width_tick, width_id), "width instant") ||
        !expect(width_tick.samples.empty(), "width no motion sample") ||
        !expect(width_only.empty() && width_only.settled() == 0, "width no fill")) {
        return 7;
    }

    reduced.set_reduced_motion(false);
    const AnimationId restored_id =
        start(reduced, opacity_block(), 2.0, 1.0, 3.0, Direction::Alternate, FillMode::Forwards);
    if (!expect(!reduced.reduced_motion(), "reduced disabled") ||
        !expect(reduced.entries().back().id == restored_id, "restored id") ||
        !expect(near(reduced.entries().back().timing.duration, 2.0), "restored duration") ||
        !expect(reduced.entries().back().timing.direction == Direction::Alternate,
                "restored direction")) {
        return 8;
    }

    SpringRuntime spring;
    spring.set_reduced_motion(true);
    SpringAnimationInput spring_input;
    spring_input.target = 2;
    spring_input.property = "x";
    spring_input.from = 0.0f;
    spring_input.to = 10.0f;
    int spring_callbacks = 0;
    spring_input.on_complete = [&](AnimationId) { ++spring_callbacks; };
    const AnimationId spring_id = spring.start(spring_input);
    const auto spring_tick = spring.tick(0.016);
    const auto* spring_sample = sample_for(spring_tick, spring_id);
    const std::string* spring_x = value_for(spring_sample, "x");
    if (!expect(spring.reduced_motion(), "spring reduced") ||
        !expect(finished_has(spring_tick, spring_id), "spring finishes immediately") ||
        !expect(spring_x != nullptr && near(std::stof(*spring_x), 10.0),
                "spring snaps to target") ||
        !expect(!spring_tick.keep_awake && spring_callbacks == 1, "spring callback once")) {
        return 9;
    }

    SpringRuntime retargeted;
    retargeted.set_reduced_motion(true);
    const AnimationId retarget_id = retargeted.start(spring_input);
    if (!expect(retargeted.retarget(retarget_id, 20.0f), "spring retarget accepted")) {
        return 10;
    }
    const auto retarget_tick = retargeted.tick(0.016);
    const auto* retarget_sample = sample_for(retarget_tick, retarget_id);
    const std::string* retarget_x = value_for(retarget_sample, "x");
    if (!expect(finished_has(retarget_tick, retarget_id), "retarget finishes") ||
        !expect(retarget_x != nullptr && near(std::stof(*retarget_x), 20.0), "retarget snaps")) {
        return 11;
    }

    SpringRuntime physical;
    if (!expect(!physical.reduced_motion(), "spring full default")) {
        return 12;
    }
    const AnimationId physical_id = physical.start(spring_input);
    const auto physical_tick = physical.tick(0.016);
    const auto* physical_sample = sample_for(physical_tick, physical_id);
    const std::string* physical_x = value_for(physical_sample, "x");
    if (!expect(physical_x != nullptr && std::stof(*physical_x) < 10.0, "physical spring moves") ||
        !expect(physical_tick.finished.empty() && physical_tick.keep_awake,
                "physical spring active")) {
        return 13;
    }

    std::cout << "animation_reduced_motion: ok\n";
    return 0;
}
