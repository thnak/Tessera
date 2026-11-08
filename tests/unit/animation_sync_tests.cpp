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
using tessera::ui::animation::StyleAnimationSync;
using tessera::ui::animation::sync_style_animations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_sync: " << message << "\n";
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

ComputedStyle none_style() {
    ComputedStyle style;
    style.set("animation-name", "none");
    return style;
}

bool sync_is(const StyleAnimationSync& result, bool ok, std::size_t started, std::size_t stopped) {
    return result.ok == ok && result.started == started && result.stopped == stopped;
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
    const auto first = sync_style_animations(runtime, 7, two_style("running, running"), blocks);
    if (!expect(sync_is(first, true, 2, 0), "initial sync") ||
        !expect(first.ids.size() == 2, "initial ids") ||
        !expect(runtime.active() == 2, "initial active")) {
        return 1;
    }

    const auto again = sync_style_animations(runtime, 7, two_style("running, running"), blocks);
    if (!expect(sync_is(again, true, 0, 0), "repeat no-op") ||
        !expect(again.ids == first.ids, "ids reused") ||
        !expect(runtime.active() == 2, "no duplicates")) {
        return 2;
    }

    const auto removed = sync_style_animations(runtime, 7, single_style("fade", "running"), blocks);
    if (!expect(sync_is(removed, true, 0, 1), "spin removed") ||
        !expect(removed.ids.size() == 1 && removed.ids[0] == first.ids[0], "fade kept") ||
        !expect(runtime.active() == 1, "one active") ||
        !expect(!runtime.stop(first.ids[1]), "spin stopped")) {
        return 3;
    }

    const auto paused = sync_style_animations(runtime, 7, single_style("fade", "paused"), blocks);
    if (!expect(sync_is(paused, true, 0, 0), "pause only") ||
        !expect(paused.ids == removed.ids, "pause id") ||
        !expect(runtime.entries().front().paused, "fade paused")) {
        return 4;
    }

    const auto restored = sync_style_animations(runtime, 7, two_style("running, running"), blocks);
    if (!expect(sync_is(restored, true, 1, 0), "spin restarts") ||
        !expect(restored.ids.size() == 2, "two ids") ||
        !expect(restored.ids[0] == first.ids[0], "fade id kept") ||
        !expect(restored.ids[1] != first.ids[1], "new spin id") ||
        !expect(!runtime.entries()[0].paused, "fade resumed") ||
        !expect(runtime.active() == 2, "two active")) {
        return 5;
    }

    const auto cleared = sync_style_animations(runtime, 7, none_style(), blocks);
    if (!expect(sync_is(cleared, true, 0, 2), "none stops active") ||
        !expect(cleared.ids.empty(), "none ids") ||
        !expect(runtime.empty(), "none clears active")) {
        return 6;
    }

    ComputedStyle fill_style = single_style("fade", "running");
    fill_style.set("animation-fill-mode", "forwards");
    AnimationRuntime fill;
    if (!expect(sync_is(sync_style_animations(fill, 8, fill_style, blocks), true, 1, 0),
                "fill start")) {
        return 7;
    }
    (void)fill.tick(1.0);
    if (!expect(fill.settled_entries().size() == 1, "fill retained") ||
        !expect(fill.settled_entries().front().keyframes.name == "fade", "fill name")) {
        return 8;
    }
    const auto fill_cleared = sync_style_animations(fill, 8, none_style(), blocks);
    if (!expect(sync_is(fill_cleared, true, 0, 1), "fill removed") ||
        !expect(fill.settled() == 0, "fill cleared")) {
        return 9;
    }

    AnimationRuntime invalid;
    (void)sync_style_animations(invalid, 9, single_style("fade", "running"), blocks);
    (void)invalid.tick(0.25);
    const AnimationId invalid_id = invalid.entries().front().id;
    ComputedStyle bad;
    bad.set("animation-name", "fade");
    bad.set("animation-duration", "-1s");
    const auto bad_result = sync_style_animations(invalid, 9, bad, blocks);
    if (!expect(sync_is(bad_result, false, 0, 0), "invalid no change") ||
        !expect(bad_result.ids.empty(), "invalid ids") ||
        !expect(invalid.active() == 1, "invalid keeps active")) {
        return 10;
    }
    const auto after_bad = invalid.tick(0.25);
    const std::string* invalid_value = value_for(after_bad, invalid_id);
    if (!expect(invalid_value != nullptr && near(std::stof(*invalid_value), 0.5f),
                "invalid does not restart")) {
        return 11;
    }

    AnimationRuntime targets;
    (void)sync_style_animations(targets, 10, single_style("fade", "running"), blocks);
    (void)sync_style_animations(targets, 11, single_style("fade", "running"), blocks);
    const auto target_cleared = sync_style_animations(targets, 10, none_style(), blocks);
    if (!expect(sync_is(target_cleared, true, 0, 1), "one target stops") ||
        !expect(targets.active() == 1 && targets.entries().front().target == 11,
                "other target kept")) {
        return 12;
    }

    AnimationRuntime case_only;
    (void)sync_style_animations(case_only, 12, single_style("fade", "running"), blocks);
    const auto case_result =
        sync_style_animations(case_only, 12, single_style("Fade", "running"), blocks);
    if (!expect(sync_is(case_result, true, 0, 1), "case-sensitive removal") ||
        !expect(case_result.ids.empty(), "case no start") ||
        !expect(case_only.empty(), "case cleared")) {
        return 13;
    }

    AnimationRuntime missing;
    (void)sync_style_animations(missing, 13, single_style("fade", "running"), blocks);
    const auto missing_result =
        sync_style_animations(missing, 13, single_style("missing", "running"), blocks);
    if (!expect(sync_is(missing_result, true, 0, 1), "missing block removes old") ||
        !expect(missing_result.ids.empty(), "missing no ids") ||
        !expect(missing.empty(), "missing settled")) {
        return 14;
    }

    std::cout << "animation_sync: ok\n";
    return 0;
}
