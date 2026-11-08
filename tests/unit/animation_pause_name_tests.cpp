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
        std::cerr << "animation_pause_name: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock block(std::string name, std::string from, std::string to) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", std::move(from), false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", std::move(to), false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, KeyframeBlock keyframes,
                  FillMode fill = FillMode::None, double duration = 1.0,
                  tessera::ui::animation::AnimationCallback callback = {}) {
    KeyframeTiming timing;
    timing.duration = duration;
    timing.fill = fill;
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(keyframes);
    input.timing = timing;
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
}

const std::string* value_for(const AnimationTickResult& result, AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id != id) {
            continue;
        }
        for (const auto& declaration : sample.declarations) {
            if (declaration.property == "opacity") {
                return &declaration.value;
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    AnimationRuntime runtime;
    int completed = 0;
    const auto callback = [&](AnimationId) { ++completed; };
    const AnimationId fade =
        start(runtime, 1, block("fade", "0", "1"), FillMode::None, 1.0, callback);
    const AnimationId spin =
        start(runtime, 1, block("spin", "0", "1"), FillMode::None, 1.0, callback);
    const AnimationId other_target =
        start(runtime, 2, block("fade", "0", "1"), FillMode::None, 1.0, callback);

    if (!expect(runtime.pause_target_name(1, "fade") == 1, "pause name count") ||
        !expect(runtime.pause_target_name(1, "fade") == 1, "pause idempotent") ||
        !expect(runtime.entries()[0].paused, "fade paused") ||
        !expect(!runtime.entries()[1].paused, "spin running") ||
        !expect(!runtime.entries()[2].paused, "other target running") ||
        !expect(runtime.pause_target_name(1, "") == 0, "empty name") ||
        !expect(runtime.pause_target_name(1, "Fade") == 0, "case sensitive") ||
        !expect(runtime.pause_target_name(1, "missing") == 0, "unknown name")) {
        return 1;
    }

    const auto held = runtime.tick(0.5);
    const std::string* held_fade = value_for(held, fade);
    const std::string* running_spin = value_for(held, spin);
    const std::string* running_other = value_for(held, other_target);
    if (!expect(held_fade != nullptr && near(std::stof(*held_fade), 0.0f), "fade held") ||
        !expect(running_spin != nullptr && near(std::stof(*running_spin), 0.5f), "spin runs") ||
        !expect(running_other != nullptr && near(std::stof(*running_other), 0.5f), "other runs") ||
        !expect(held.keep_awake, "siblings wake") || !expect(completed == 0, "no completion")) {
        return 2;
    }

    (void)runtime.pause_target_name(1, "fade");
    const auto middle = runtime.tick(0.5);
    const std::string* held_again = value_for(middle, fade);
    if (!expect(held_again != nullptr && near(std::stof(*held_again), 0.0f), "fade still held") ||
        !expect(middle.finished.size() == 2, "siblings finish") ||
        !expect(completed == 2, "two callbacks") ||
        !expect(runtime.active() == 1, "fade remains") ||
        !expect(!middle.keep_awake, "paused only sleeps")) {
        return 3;
    }

    if (!expect(runtime.resume_target_name(1, "fade") == 1, "resume name") ||
        !expect(runtime.resume_target_name(1, "fade") == 1, "resume idempotent") ||
        !expect(runtime.resume_target_name(1, "") == 0, "empty resume") ||
        !expect(runtime.resume_target_name(1, "Fade") == 0, "resume case") ||
        !expect(runtime.resume_target_name(1, "missing") == 0, "resume unknown")) {
        return 4;
    }

    const auto resumed = runtime.tick(0.5);
    const std::string* resumed_fade = value_for(resumed, fade);
    if (!expect(resumed_fade != nullptr && near(std::stof(*resumed_fade), 0.5f), "fade resumes") ||
        !expect(resumed.keep_awake, "resumed wakes")) {
        return 5;
    }
    const auto done = runtime.tick(0.5);
    if (!expect(done.finished.size() == 1 && done.finished[0] == fade, "fade finishes") ||
        !expect(completed == 3, "fade callback") || !expect(!done.keep_awake, "settles")) {
        return 6;
    }

    AnimationRuntime sleeping;
    (void)start(sleeping, 3, block("fade", "0", "1"));
    (void)start(sleeping, 3, block("spin", "0", "1"));
    if (!expect(sleeping.pause_target_name(3, "fade") == 1, "sleep fade") ||
        !expect(sleeping.pause_target_name(3, "spin") == 1, "sleep spin")) {
        return 7;
    }
    const auto sleeping_tick = sleeping.tick(0.5);
    if (!expect(sleeping_tick.samples.size() == 2, "two held samples") ||
        !expect(!sleeping_tick.keep_awake, "all paused sleeps")) {
        return 8;
    }
    if (!expect(sleeping.resume_target_name(3, "fade") == 1, "resume one") ||
        !expect(sleeping.resume_target_name(3, "fade") == 1, "resume one again")) {
        return 9;
    }
    const auto wake = sleeping.tick(0.5);
    if (!expect(wake.keep_awake, "one resumed wakes")) {
        return 10;
    }

    AnimationRuntime fill;
    (void)start(fill, 4, block("fade", "0", "1"), FillMode::Forwards);
    (void)fill.tick(1.0);
    if (!expect(fill.settled() == 1, "fill exists") ||
        !expect(fill.pause_target_name(4, "fade") == 0, "fill pause ignored") ||
        !expect(fill.resume_target_name(4, "fade") == 0, "fill resume ignored") ||
        !expect(fill.settled() == 1, "fill preserved")) {
        return 11;
    }

    AnimationRuntime running;
    (void)start(running, 5, block("fade", "0", "1"));
    if (!expect(running.resume_target_name(5, "fade") == 1, "resume running count") ||
        !expect(running.tick(0.5).keep_awake, "running stays awake")) {
        return 12;
    }

    std::cout << "animation_pause_name: ok\n";
    return 0;
}
