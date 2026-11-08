// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_stop: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

Declaration declaration(std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), false, 0};
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {declaration("opacity", "0")}});
    block.steps.push_back(KeyframeStep{100.0, {declaration("opacity", "1")}});
    return block;
}

KeyframeBlock width_block() {
    KeyframeBlock block;
    block.name = "grow";
    block.steps.push_back(KeyframeStep{0.0, {declaration("width", "10px")}});
    block.steps.push_back(KeyframeStep{100.0, {declaration("width", "20px")}});
    return block;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, KeyframeBlock block,
                  tessera::ui::animation::AnimationCallback callback = {}) {
    KeyframeTiming timing;
    timing.duration = 1.0;
    timing.fill = FillMode::Forwards;
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(block);
    input.timing = timing;
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
}

const std::string* value_for(const AnimationTickResult& result, AnimationId id,
                             std::string_view property) {
    for (const auto& sample : result.samples) {
        if (sample.id != id) {
            continue;
        }
        for (const auto& declaration : sample.declarations) {
            if (declaration.property == property) {
                return &declaration.value;
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    AnimationRuntime runtime;
    int completions = 0;
    const auto callback = [&](AnimationId) { ++completions; };
    const AnimationId fading = start(runtime, 7, opacity_block(), callback);
    const AnimationId growing = start(runtime, 7, width_block(), callback);
    const AnimationId other = start(runtime, 8, opacity_block(), callback);

    if (!expect(runtime.pause(growing), "pause growing") ||
        !expect(runtime.pause(other), "pause other")) {
        return 1;
    }

    if (!expect(runtime.stop_target(7) == 2, "stop target count") ||
        !expect(runtime.active() == 1, "other target remains") ||
        !expect(runtime.stop_target(7) == 0, "stop twice") ||
        !expect(runtime.stop_target(99) == 0, "unknown target") ||
        !expect(runtime.entries().front().target == 8, "preserved target") ||
        !expect(runtime.entries().front().paused, "preserved pause state")) {
        return 2;
    }

    const auto held = runtime.tick(0.5);
    const std::string* held_value = value_for(held, other, "opacity");
    if (!expect(held.samples.size() == 1, "one preserved sample") ||
        !expect(held.samples.front().target == 8, "preserved sample target") ||
        !expect(held_value != nullptr && near(std::stof(*held_value), 0.0f),
                "preserved sample value") ||
        !expect(held.finished.empty(), "no completion") ||
        !expect(!held.keep_awake, "paused sleeps") || !expect(completions == 0, "no callback")) {
        return 3;
    }

    if (!expect(runtime.resume(other), "resume other")) {
        return 4;
    }
    const auto running = runtime.tick(0.5);
    const std::string* running_value = value_for(running, other, "opacity");
    if (!expect(running_value != nullptr && near(std::stof(*running_value), 0.5f),
                "preserved animation continues") ||
        !expect(running.keep_awake, "preserved keeps awake")) {
        return 5;
    }

    if (!expect(runtime.stop_target(8) == 1, "stop preserved") ||
        !expect(runtime.empty(), "empty after stop")) {
        return 6;
    }
    const auto empty_tick = runtime.tick(0.5);
    if (!expect(empty_tick.samples.empty(), "no samples") ||
        !expect(empty_tick.finished.empty(), "no finish") ||
        !expect(!empty_tick.keep_awake, "sleeps") ||
        !expect(completions == 0, "still no callback")) {
        return 7;
    }

    AnimationRuntime zero;
    (void)start(zero, 0, opacity_block());
    if (!expect(zero.stop_target(0) == 1, "target zero") || !expect(zero.empty(), "zero empty")) {
        return 8;
    }

    (void)fading;
    (void)growing;
    std::cout << "animation_stop: ok\n";
    return 0;
}
