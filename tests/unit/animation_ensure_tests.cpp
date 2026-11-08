// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::ensure_style_animations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_ensure: " << message << "\n";
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

ComputedStyle two_style(std::string play_states) {
    ComputedStyle style;
    style.set("animation-name", "fade, spin");
    style.set("animation-duration", "1s, 1s");
    style.set("animation-timing-function", "linear");
    style.set("animation-play-state", std::move(play_states));
    return style;
}

ComputedStyle single_style(std::string name, std::string play_state) {
    ComputedStyle style;
    style.set("animation-name", std::move(name));
    style.set("animation-duration", "1s");
    style.set("animation-timing-function", "linear");
    style.set("animation-play-state", std::move(play_state));
    return style;
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
    const std::vector<KeyframeBlock> blocks = {block("fade"), block("spin")};

    AnimationRuntime runtime;
    const auto first = ensure_style_animations(runtime, 7, two_style("running, running"), blocks);
    const auto second = ensure_style_animations(runtime, 7, two_style("running, running"), blocks);
    if (!expect(first.size() == 2, "first starts two") ||
        !expect(second.size() == 2 && second == first, "ids reused") ||
        !expect(runtime.active() == 2, "no duplicates")) {
        return 1;
    }

    const auto paused = ensure_style_animations(runtime, 7, two_style("paused, running"), blocks);
    if (!expect(paused.size() == 2 && paused == first, "paused ids reused") ||
        !expect(runtime.entries()[0].paused, "fade paused") ||
        !expect(!runtime.entries()[1].paused, "spin running")) {
        return 2;
    }

    const auto held = runtime.tick(0.25);
    const std::string* held_fade = value_for(held, first[0]);
    const std::string* running_spin = value_for(held, first[1]);
    if (!expect(held_fade != nullptr && near(std::stof(*held_fade), 0.0f), "fade held") ||
        !expect(running_spin != nullptr && near(std::stof(*running_spin), 0.25f), "spin runs")) {
        return 3;
    }

    const auto resumed = ensure_style_animations(runtime, 7, two_style("running, running"), blocks);
    if (!expect(resumed.size() == 2 && resumed == first, "resume ids reused") ||
        !expect(!runtime.entries()[0].paused, "fade resumed")) {
        return 4;
    }
    const auto resumed_tick = runtime.tick(0.25);
    const std::string* resumed_fade = value_for(resumed_tick, first[0]);
    const std::string* continued_spin = value_for(resumed_tick, first[1]);
    if (!expect(resumed_fade != nullptr && near(std::stof(*resumed_fade), 0.25f), "fade resumes") ||
        !expect(continued_spin != nullptr && near(std::stof(*continued_spin), 0.5f),
                "spin continues")) {
        return 5;
    }
    const auto done = runtime.tick(0.75);
    if (!expect(done.finished.size() == 2 && runtime.empty(), "both finish")) {
        return 6;
    }

    AnimationRuntime retained;
    const auto both = ensure_style_animations(retained, 8, two_style("running, running"), blocks);
    const auto fade_only =
        ensure_style_animations(retained, 8, single_style("fade", "running"), blocks);
    if (!expect(both.size() == 2, "retained both") ||
        !expect(fade_only.size() == 1 && fade_only[0] == both[0], "fade id reused") ||
        !expect(retained.active() == 2, "other name retained")) {
        return 7;
    }

    ComputedStyle fill_style = single_style("fade", "running");
    fill_style.set("animation-fill-mode", "forwards");
    AnimationRuntime fill;
    if (!expect(ensure_style_animations(fill, 9, fill_style, blocks).size() == 1, "fill starts") ||
        !expect(fill.active() == 1, "fill active")) {
        return 8;
    }
    (void)fill.tick(1.0);
    if (!expect(fill.settled() == 1, "fill settled") ||
        !expect(ensure_style_animations(fill, 9, fill_style, blocks).empty(),
                "fill not restarted") ||
        !expect(fill.active() == 0 && fill.settled() == 1, "fill preserved")) {
        return 9;
    }

    ComputedStyle shorthand;
    shorthand.set("animation", "fade 1s linear");
    AnimationRuntime shorthand_runtime;
    if (!expect(ensure_style_animations(shorthand_runtime, 10, shorthand, blocks).size() == 1,
                "shorthand starts") ||
        !expect(shorthand_runtime.active() == 1, "shorthand active")) {
        return 10;
    }

    AnimationRuntime invalid;
    (void)ensure_style_animations(invalid, 11, single_style("fade", "running"), blocks);
    const auto before_invalid = invalid.tick(0.25);
    const AnimationId invalid_id = invalid.entries().front().id;
    ComputedStyle bad;
    bad.set("animation-name", "fade");
    bad.set("animation-duration", "-1s");
    bad.set("animation-play-state", "paused");
    if (!expect(ensure_style_animations(invalid, 11, bad, blocks).empty(), "invalid empty") ||
        !expect(invalid.active() == 1, "invalid no start")) {
        return 11;
    }
    (void)before_invalid;
    const auto after_invalid = invalid.tick(0.25);
    const std::string* invalid_value = value_for(after_invalid, invalid_id);
    if (!expect(invalid_value != nullptr && near(std::stof(*invalid_value), 0.5f),
                "invalid does not restart")) {
        return 12;
    }

    ComputedStyle missing = single_style("missing", "running");
    ComputedStyle none;
    none.set("animation-name", "none");
    AnimationRuntime empty;
    if (!expect(ensure_style_animations(empty, 12, missing, blocks).empty(), "missing block") ||
        !expect(ensure_style_animations(empty, 12, none, blocks).empty(), "none skipped") ||
        !expect(empty.empty(), "nothing started")) {
        return 13;
    }

    AnimationRuntime paused_start;
    const auto paused_ids =
        ensure_style_animations(paused_start, 13, single_style("fade", "paused"), blocks);
    if (!expect(paused_ids.size() == 1, "paused starts") ||
        !expect(paused_start.entries().front().paused, "paused entry") ||
        !expect(!paused_start.tick(0.5).keep_awake, "paused sleeps")) {
        return 14;
    }

    std::cout << "animation_ensure: ok\n";
    return 0;
}
