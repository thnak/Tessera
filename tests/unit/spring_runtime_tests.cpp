// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/spring_runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::Dirty;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::SpringAnimationInput;
using tessera::ui::animation::SpringConfig;
using tessera::ui::animation::SpringRuntime;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "spring_runtime: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

float sample_value(const AnimationTickResult& result, AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id == id && !sample.declarations.empty()) {
            return std::stof(sample.declarations.front().value);
        }
    }
    return -1.0f;
}

SpringAnimationInput input(std::string property, float from, float to) {
    SpringAnimationInput result;
    result.target = 7;
    result.property = std::move(property);
    result.from = from;
    result.to = to;
    result.config = SpringConfig{180.0f, 20.0f, 1.0f, 0.01f, 0.01f};
    return result;
}

} // namespace

int main() {
    SpringRuntime runtime;
    const AnimationId opacity = runtime.start(input("opacity", 0.0f, 1.0f));
    if (!expect(opacity != 0, "opacity id") || !expect(runtime.active() == 1, "opacity active")) {
        return 1;
    }
    const auto first = runtime.tick(0.016);
    if (!expect(first.samples.size() == 1, "opacity sample") ||
        !expect(sample_value(first, opacity) > 0.0f, "opacity moved") ||
        !expect(has_flag(first.dirty, Dirty::Paint), "opacity paint dirty") ||
        !expect(!has_flag(first.dirty, Dirty::Layout), "opacity not layout") ||
        !expect(first.keep_awake, "opacity keeps awake")) {
        return 2;
    }

    AnimationTickResult last;
    int ticks = 0;
    while (!runtime.empty() && ticks < 1000) {
        last = runtime.tick(0.016);
        ++ticks;
    }
    if (!expect(runtime.empty(), "opacity settled") ||
        !expect(last.finished.size() == 1 && last.finished[0] == opacity, "opacity finished") ||
        !expect(!last.keep_awake, "opacity released wake") ||
        !expect(near(sample_value(last, opacity), 1.0f), "opacity final")) {
        return 3;
    }

    const AnimationId width = runtime.start(input("width", 0.0f, 100.0f));
    const auto width_tick = runtime.tick(0.016);
    if (!expect(width != 0, "width id") ||
        !expect(has_flag(width_tick.dirty, Dirty::Layout), "width layout dirty") ||
        !expect(has_flag(width_tick.dirty, Dirty::Paint), "width paint dirty")) {
        return 4;
    }
    runtime.clear();
    if (!expect(runtime.empty(), "clear springs")) {
        return 5;
    }

    const AnimationId retargeted = runtime.start(input("opacity", 0.0f, 1.0f));
    (void)runtime.tick(0.1);
    if (!expect(runtime.retarget(retargeted, 2.0f), "retarget found") ||
        !expect(!runtime.retarget(retargeted, std::nanf("")), "retarget nan rejected") ||
        !expect(!runtime.retarget(999, 1.0f), "retarget missing")) {
        return 6;
    }
    ticks = 0;
    while (!runtime.empty() && ticks < 2000) {
        last = runtime.tick(0.016);
        ++ticks;
    }
    if (!expect(near(sample_value(last, retargeted), 2.0f), "retarget final")) {
        return 7;
    }

    SpringRuntime callback_runtime;
    AnimationId followup = 0;
    const AnimationId source = callback_runtime.start(SpringAnimationInput{
        1, "opacity", 0.0f, 1.0f, SpringConfig{180.0f, 20.0f, 1.0f, 0.01f, 0.01f},
        [&](AnimationId) { followup = callback_runtime.start(input("opacity", 0.0f, 1.0f)); }});
    const AnimationId stopped = callback_runtime.start(input("opacity", 0.0f, 1.0f));
    if (!expect(callback_runtime.stop(stopped), "stop existing") ||
        !expect(!callback_runtime.stop(stopped), "stop missing")) {
        return 8;
    }
    ticks = 0;
    AnimationTickResult callback_tick;
    while (followup == 0 && ticks < 1000) {
        callback_tick = callback_runtime.tick(0.016);
        ++ticks;
    }
    if (!expect(source != 0 && followup != 0, "callback starts followup") ||
        !expect(callback_tick.keep_awake, "followup keeps awake")) {
        return 9;
    }
    callback_runtime.clear();

    SpringRuntime invalid_runtime;
    const AnimationId invalid = invalid_runtime.start(input("opacity", 0.0f, 1.0f));
    const auto bad_tick = invalid_runtime.tick(std::nan(""));
    if (!expect(invalid != 0, "invalid id") ||
        !expect(bad_tick.samples.empty(), "nan dt ignored") ||
        !expect(bad_tick.keep_awake, "nan dt keeps awake") ||
        !expect(invalid_runtime.active() == 1, "nan dt leaves active")) {
        return 10;
    }

    std::cout << "spring_runtime: ok\n";
    return 0;
}
