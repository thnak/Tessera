// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::apply_style_play_state;
using tessera::ui::animation::start_style_animations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_play_state: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock block(std::string name) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return result;
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

ComputedStyle single(std::string play_state) {
    ComputedStyle style;
    style.set("animation-name", "fade");
    style.set("animation-duration", "1s");
    style.set("animation-timing-function", "linear");
    style.set("animation-play-state", std::move(play_state));
    return style;
}

} // namespace

int main() {
    const std::vector<KeyframeBlock> blocks = {block("fade"), block("spin")};

    ComputedStyle running;
    running.set("animation-name", "fade, spin");
    running.set("animation-duration", "1s, 1s");
    running.set("animation-timing-function", "linear");
    running.set("animation-play-state", "running, running");

    AnimationRuntime runtime;
    const auto ids = start_style_animations(runtime, 7, running, blocks);
    if (!expect(ids.size() == 2, "started two") || !expect(runtime.active() == 2, "two active")) {
        return 1;
    }

    const ComputedStyle paused = single("paused");
    const ComputedStyle resumed = single("running");
    if (!expect(apply_style_play_state(runtime, 7, paused) == 1, "pause changed") ||
        !expect(runtime.entries()[0].paused, "fade paused") ||
        !expect(!runtime.entries()[1].paused, "spin running") ||
        !expect(apply_style_play_state(runtime, 7, paused) == 0, "pause idempotent")) {
        return 2;
    }

    const auto held = runtime.tick(0.25);
    const std::string* held_fade = value_for(held, ids[0]);
    const std::string* running_spin = value_for(held, ids[1]);
    if (!expect(held_fade != nullptr && near(std::stof(*held_fade), 0.0f), "fade held") ||
        !expect(running_spin != nullptr && near(std::stof(*running_spin), 0.25f), "spin runs") ||
        !expect(held.keep_awake, "spin wakes")) {
        return 3;
    }

    if (!expect(apply_style_play_state(runtime, 7, resumed) == 1, "resume changed") ||
        !expect(apply_style_play_state(runtime, 7, resumed) == 0, "resume idempotent")) {
        return 4;
    }
    const auto resumed_tick = runtime.tick(0.25);
    const std::string* resumed_fade = value_for(resumed_tick, ids[0]);
    const std::string* resumed_spin = value_for(resumed_tick, ids[1]);
    if (!expect(resumed_fade != nullptr && near(std::stof(*resumed_fade), 0.25f), "fade resumes") ||
        !expect(resumed_spin != nullptr && near(std::stof(*resumed_spin), 0.5f),
                "spin continues")) {
        return 5;
    }

    ComputedStyle invalid;
    invalid.set("animation-name", "fade");
    invalid.set("animation-duration", "-1s");
    invalid.set("animation-play-state", "paused");
    if (!expect(apply_style_play_state(runtime, 7, invalid) == 0, "invalid longhand") ||
        !expect(!runtime.entries()[0].paused, "invalid leaves running")) {
        return 6;
    }

    ComputedStyle invalid_shorthand;
    invalid_shorthand.set("animation", "fade bogus");
    if (!expect(apply_style_play_state(runtime, 7, invalid_shorthand) == 0, "invalid shorthand") ||
        !expect(!runtime.entries()[0].paused, "invalid shorthand unchanged")) {
        return 7;
    }

    ComputedStyle precedence;
    precedence.set("animation", "fade 1s linear running");
    precedence.set("animation-name", "fade");
    precedence.set("animation-duration", "1s");
    precedence.set("animation-play-state", "paused");
    if (!expect(apply_style_play_state(runtime, 7, precedence) == 1, "longhands win") ||
        !expect(runtime.entries()[0].paused, "longhand paused")) {
        return 8;
    }

    ComputedStyle shorthand_running;
    shorthand_running.set("animation", "fade 1s linear running");
    if (!expect(apply_style_play_state(runtime, 7, shorthand_running) == 1, "shorthand resumes") ||
        !expect(!runtime.entries()[0].paused, "shorthand running")) {
        return 9;
    }

    ComputedStyle shorthand_paused;
    shorthand_paused.set("animation", "fade 1s linear paused");
    if (!expect(apply_style_play_state(runtime, 7, shorthand_paused) == 1, "shorthand pauses") ||
        !expect(runtime.entries()[0].paused, "shorthand paused") ||
        !expect(apply_style_play_state(runtime, 7, shorthand_paused) == 0,
                "shorthand idempotent")) {
        return 10;
    }

    ComputedStyle missing = single("paused");
    missing.set("animation-name", "missing");
    if (!expect(apply_style_play_state(runtime, 7, missing) == 0, "unknown name")) {
        return 11;
    }

    AnimationRuntime other;
    (void)start_style_animations(other, 8, running, blocks);
    if (!expect(apply_style_play_state(other, 99, paused) == 0, "other target") ||
        !expect(!other.entries()[0].paused, "other target untouched")) {
        return 12;
    }

    ComputedStyle fill_style = single("running");
    fill_style.set("animation-fill-mode", "forwards");
    AnimationRuntime fill;
    (void)start_style_animations(fill, 9, fill_style, blocks);
    (void)fill.tick(1.0);
    if (!expect(fill.settled() == 1, "fill settled") ||
        !expect(apply_style_play_state(fill, 9, paused) == 0, "fill ignored") ||
        !expect(fill.settled() == 1, "fill preserved")) {
        return 13;
    }

    std::cout << "animation_play_state: ok\n";
    return 0;
}
