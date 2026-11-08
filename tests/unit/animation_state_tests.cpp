// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationNameState;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_state: " << message << "\n";
    }
    return condition;
}

bool state_is(const AnimationNameState& state, std::size_t active, std::size_t settled) {
    return state.active == active && state.settled == settled;
}

KeyframeBlock block(std::string name, std::string from, std::string to) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", std::move(from), false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", std::move(to), false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, KeyframeBlock keyframes,
                  FillMode fill = FillMode::None, double duration = 1.0) {
    KeyframeTiming timing;
    timing.duration = duration;
    timing.fill = fill;
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(keyframes);
    input.timing = timing;
    return runtime.start(std::move(input));
}

} // namespace

int main() {
    AnimationRuntime empty;
    const AnimationRuntime& const_empty = empty;
    if (!expect(state_is(const_empty.target_name_state(1, "fade"), 0, 0), "empty runtime") ||
        !expect(state_is(empty.target_name_state(1, ""), 0, 0), "empty name")) {
        return 1;
    }

    AnimationRuntime runtime;
    const AnimationId first = start(runtime, 1, block("fade", "0", "1"));
    (void)start(runtime, 1, block("fade", "0", "1"));
    (void)start(runtime, 1, block("spin", "0", "1"));
    (void)start(runtime, 2, block("fade", "0", "1"));

    const AnimationRuntime& const_runtime = runtime;
    if (!expect(state_is(const_runtime.target_name_state(1, "fade"), 2, 0), "two active") ||
        !expect(state_is(const_runtime.target_name_state(1, "spin"), 1, 0), "sibling name") ||
        !expect(state_is(const_runtime.target_name_state(2, "fade"), 1, 0), "other target") ||
        !expect(state_is(const_runtime.target_name_state(1, "Fade"), 0, 0), "case sensitive") ||
        !expect(state_is(const_runtime.target_name_state(1, "missing"), 0, 0), "unknown name") ||
        !expect(state_is(const_runtime.target_name_state(1, ""), 0, 0), "empty query") ||
        !expect(runtime.active() == 4, "query does not mutate")) {
        return 2;
    }

    (void)runtime.pause(first);
    if (!expect(state_is(runtime.target_name_state(1, "fade"), 2, 0), "paused counted")) {
        return 3;
    }

    AnimationRuntime fill;
    (void)start(fill, 3, block("fade", "0", "1"), FillMode::Forwards);
    (void)fill.tick(1.0);
    if (!expect(state_is(fill.target_name_state(3, "fade"), 0, 1), "settled fill") ||
        !expect(state_is(fill.target_name_state(3, "Fade"), 0, 0), "fill case sensitive") ||
        !expect(state_is(fill.target_name_state(3, ""), 0, 0), "fill empty name")) {
        return 4;
    }

    AnimationRuntime mixed;
    (void)start(mixed, 4, block("fade", "0", "1"), FillMode::Forwards, 1.0);
    (void)start(mixed, 4, block("fade", "0", "1"), FillMode::Forwards, 2.0);
    (void)mixed.tick(1.0);
    if (!expect(state_is(mixed.target_name_state(4, "fade"), 1, 1), "active and settled") ||
        !expect(mixed.stop_target_name(4, "fade") == 2, "cleanup both") ||
        !expect(state_is(mixed.target_name_state(4, "fade"), 0, 0), "cleared state")) {
        return 5;
    }

    std::cout << "animation_state: ok\n";
    return 0;
}
