// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_start.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationSpec;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::parse_animation_list;
using tessera::ui::animation::start_animation;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_start: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock opacity_block(std::string name = "spin") {
    KeyframeBlock block;
    block.name = std::move(name);
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

const std::string* value(const AnimationTickResult& result) {
    for (const auto& sample : result.samples) {
        for (const auto& declaration : sample.declarations) {
            if (declaration.property == "opacity") {
                return &declaration.value;
            }
        }
    }
    return nullptr;
}

AnimationSpec parse_one(const char* text) {
    const auto parsed = parse_animation_list(text);
    if (!parsed.ok || parsed.animations.size() != 1) {
        return {};
    }
    return parsed.animations.front();
}

} // namespace

int main() {
    AnimationRuntime runtime;
    const AnimationId plain =
        start_animation(runtime, 7, parse_one("spin 1s linear"), opacity_block());
    if (!expect(plain != 0, "plain id") || !expect(runtime.active() == 1, "plain active")) {
        return 1;
    }
    const auto quarter = runtime.tick(0.25);
    const std::string* quarter_value = value(quarter);
    if (!expect(quarter_value != nullptr && near(std::stof(*quarter_value), 0.25f),
                "quarter value") ||
        !expect(quarter.samples[0].target == 7, "target")) {
        return 2;
    }
    const auto plain_done = runtime.tick(0.75);
    if (!expect(plain_done.finished.size() == 1, "plain finished") ||
        !expect(plain_done.samples.empty(), "no fill sample") ||
        !expect(runtime.empty(), "plain settled")) {
        return 3;
    }

    AnimationRuntime fill_runtime;
    const AnimationId fill_id =
        start_animation(fill_runtime, 8, parse_one("spin 1s linear forwards"), opacity_block());
    const auto fill_done = fill_runtime.tick(1.0);
    const std::string* fill_value = value(fill_done);
    if (!expect(fill_id != 0, "fill id") ||
        !expect(fill_done.finished.size() == 1, "fill finish") ||
        !expect(fill_value != nullptr && near(std::stof(*fill_value), 1.0f), "fill final")) {
        return 4;
    }

    AnimationRuntime delay_runtime;
    const AnimationId delay_id =
        start_animation(delay_runtime, 9, parse_one("spin 0.5s linear 0.5s both"), opacity_block());
    const auto backwards = delay_runtime.tick(0.25);
    const std::string* backwards_value = value(backwards);
    if (!expect(delay_id != 0, "delay id") ||
        !expect(backwards_value != nullptr && near(std::stof(*backwards_value), 0.0f),
                "backwards fill")) {
        return 5;
    }
    (void)delay_runtime.tick(0.25);
    const auto delayed_done = delay_runtime.tick(0.5);
    const std::string* delayed_value = value(delayed_done);
    if (!expect(delayed_done.finished.size() == 1, "delay finished") ||
        !expect(delayed_value != nullptr && near(std::stof(*delayed_value), 1.0f), "delay final")) {
        return 6;
    }

    AnimationRuntime reverse_runtime;
    const AnimationId reverse_id = start_animation(
        reverse_runtime, 10, parse_one("spin 1s linear 0s 1 reverse forwards"), opacity_block());
    const auto reverse_done = reverse_runtime.tick(1.0);
    const std::string* reverse_value = value(reverse_done);
    if (!expect(reverse_id != 0, "reverse id") ||
        !expect(reverse_done.finished.size() == 1, "reverse finished") ||
        !expect(reverse_value != nullptr && near(std::stof(*reverse_value), 0.0f),
                "reverse final")) {
        return 7;
    }

    AnimationRuntime zero_duration;
    const AnimationId zero_id =
        start_animation(zero_duration, 11, parse_one("spin 0s linear forwards"), opacity_block());
    const auto zero_done = zero_duration.tick(0.0);
    const std::string* zero_value = value(zero_done);
    if (!expect(zero_id != 0, "zero duration id") ||
        !expect(zero_done.finished.size() == 1, "zero duration finished") ||
        !expect(zero_value != nullptr && near(std::stof(*zero_value), 1.0f),
                "zero duration final")) {
        return 8;
    }

    AnimationRuntime infinite_runtime;
    const AnimationId infinite_id = start_animation(
        infinite_runtime, 12, parse_one("spin 1s linear infinite"), opacity_block());
    const auto long_tick = infinite_runtime.tick(100.0);
    if (!expect(infinite_id != 0, "infinite id") ||
        !expect(long_tick.finished.empty(), "infinite stays active") ||
        !expect(infinite_runtime.active() == 1, "infinite entry")) {
        return 9;
    }

    AnimationRuntime callback_runtime;
    int completed = 0;
    const AnimationId callback_id =
        start_animation(callback_runtime, 13, parse_one("spin 1s linear forwards"), opacity_block(),
                        [&](AnimationId id) {
                            if (id == callback_id) {
                                ++completed;
                            }
                        });
    (void)callback_runtime.tick(1.0);
    if (!expect(callback_id != 0, "callback id") || !expect(completed == 1, "callback fired")) {
        return 10;
    }

    AnimationRuntime paused_runtime;
    const AnimationId paused_id =
        start_animation(paused_runtime, 14, parse_one("spin 1s linear paused"), opacity_block());
    const auto paused_hold = paused_runtime.tick(0.5);
    const std::string* paused_value = value(paused_hold);
    if (!expect(paused_id != 0, "paused id") ||
        !expect(paused_runtime.active() == 1, "paused active") ||
        !expect(paused_value != nullptr && near(std::stof(*paused_value), 0.0f), "paused held") ||
        !expect(!paused_hold.keep_awake, "paused sleeps")) {
        return 11;
    }
    (void)paused_runtime.resume(paused_id);
    const auto paused_resumed = paused_runtime.tick(0.5);
    const std::string* resumed_value = value(paused_resumed);
    if (!expect(resumed_value != nullptr && near(std::stof(*resumed_value), 0.5f),
                "paused resumes") ||
        !expect(paused_resumed.keep_awake, "resumed wakes")) {
        return 12;
    }

    AnimationRuntime skipped;
    KeyframeBlock empty;
    empty.name = "spin";
    AnimationSpec zero_iterations = parse_one("spin 1s 0");
    AnimationSpec negative;
    negative.name = "spin";
    negative.duration = -1.0f;
    AnimationSpec bad_delay;
    bad_delay.name = "spin";
    bad_delay.delay = std::nanf("");
    AnimationSpec none;

    if (!expect(start_animation(skipped, 1, none, opacity_block()) == 0, "none skipped") ||
        !expect(start_animation(skipped, 1, parse_one("spin 1s"), empty) == 0, "empty skipped") ||
        !expect(start_animation(skipped, 1, zero_iterations, opacity_block()) == 0,
                "zero iterations skipped") ||
        !expect(start_animation(skipped, 1, negative, opacity_block()) == 0,
                "negative duration skipped") ||
        !expect(start_animation(skipped, 1, bad_delay, opacity_block()) == 0,
                "bad delay skipped") ||
        !expect(start_animation(skipped, 1, parse_one("spin 1s"), opacity_block("other")) == 0,
                "name mismatch skipped") ||
        !expect(skipped.empty(), "nothing started")) {
        return 13;
    }

    std::cout << "animation_start: ok\n";
    return 0;
}
