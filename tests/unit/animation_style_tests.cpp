// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"
#include "tessera/ui/animation/style_overlay.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::start_style_animations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_style: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

KeyframeBlock width_block() {
    KeyframeBlock block;
    block.name = "spin";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"width", "10px", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"width", "20px", false, 0}}});
    return block;
}

ComputedStyle base_style() {
    ComputedStyle style;
    style.set("opacity", "1");
    style.set("width", "10px");
    style.set("height", "5px");
    return style;
}

float number(const ComputedStyle& style, const char* property, bool& found) {
    const std::string* value = style.find(property);
    found = value != nullptr;
    return found ? std::stof(*value) : -1.0f;
}

const std::string* value_for(const AnimationTickResult& result, AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id != id) {
            continue;
        }
        for (const auto& declaration : sample.declarations) {
            if (declaration.property == "opacity" || declaration.property == "width") {
                return &declaration.value;
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    const std::vector<KeyframeBlock> blocks = {opacity_block(), width_block()};

    ComputedStyle longhands;
    longhands.set("animation-name", "fade, spin");
    longhands.set("animation-duration", "1s, 2s");
    longhands.set("animation-timing-function", "linear");
    longhands.set("animation-delay", "0s");
    longhands.set("animation-iteration-count", "1");
    longhands.set("animation-direction", "normal");
    longhands.set("animation-fill-mode", "forwards");
    longhands.set("animation-play-state", "running");

    AnimationRuntime longhand_runtime;
    int completed = 0;
    const auto ids = start_style_animations(longhand_runtime, 7, longhands, blocks,
                                            [&](AnimationId) { ++completed; });
    if (!expect(ids.size() == 2, "longhand count") ||
        !expect(longhand_runtime.active() == 2, "longhand active")) {
        return 1;
    }

    ComputedStyle style = base_style();
    const auto half = longhand_runtime.tick(0.5);
    apply_animation_result(style, half, 7);
    bool found = false;
    const float half_opacity = number(style, "opacity", found);
    if (!expect(found && near(half_opacity, 0.5f), "fade halfway")) {
        return 2;
    }
    const float half_width = number(style, "width", found);
    if (!expect(found && near(half_width, 12.5f), "spin quarter")) {
        return 3;
    }
    const std::string* fade_sample = value_for(half, ids[0]);
    const std::string* spin_sample = value_for(half, ids[1]);
    if (!expect(fade_sample != nullptr && near(std::stof(*fade_sample), 0.5f), "fade sample") ||
        !expect(spin_sample != nullptr && near(std::stof(*spin_sample), 12.5f), "spin sample")) {
        return 4;
    }

    const auto middle = longhand_runtime.tick(0.5);
    if (!expect(middle.finished.size() == 1 && middle.finished[0] == ids[0], "fade done") ||
        !expect(completed == 1, "one completion") || !expect(middle.keep_awake, "stays awake")) {
        return 5;
    }
    style = base_style();
    apply_animation_result(style, middle, 7);
    const float fade_final = number(style, "opacity", found);
    const float spinning = number(style, "width", found);
    if (!expect(found && near(fade_final, 1.0f), "fill forwards") ||
        !expect(found && near(spinning, 15.0f), "spin halfway")) {
        return 6;
    }

    const auto end = longhand_runtime.tick(1.0);
    style = base_style();
    apply_animation_result(style, end, 7);
    const float final_width = number(style, "width", found);
    if (!expect(end.finished.size() == 1 && end.finished[0] == ids[1], "spin done") ||
        !expect(completed == 2, "two completions") || !expect(!end.keep_awake, "wake released") ||
        !expect(longhand_runtime.empty(), "settled") ||
        !expect(found && near(final_width, 20.0f), "final width") ||
        !expect(*style.find("height") == "5px", "height preserved")) {
        return 7;
    }

    ComputedStyle shorthand;
    shorthand.set("animation", "fade 1s linear, spin 2s linear");
    AnimationRuntime shorthand_runtime;
    if (!expect(start_style_animations(shorthand_runtime, 8, shorthand, blocks).size() == 2,
                "shorthand fallback")) {
        return 8;
    }

    ComputedStyle precedence;
    precedence.set("animation", "fade 1s linear");
    precedence.set("animation-name", "spin");
    precedence.set("animation-duration", "2s");
    precedence.set("animation-timing-function", "linear");
    AnimationRuntime precedence_runtime;
    const auto precedence_ids = start_style_animations(precedence_runtime, 9, precedence, blocks);
    if (!expect(precedence_ids.size() == 1, "longhand precedence") ||
        !expect(precedence_runtime.active() == 1, "one active")) {
        return 9;
    }
    const auto precedence_tick = precedence_runtime.tick(0.5);
    ComputedStyle precedence_style = base_style();
    apply_animation_result(precedence_style, precedence_tick, 9);
    const float precedence_width = number(precedence_style, "width", found);
    if (!expect(found && near(precedence_width, 12.5f), "longhand animation ran") ||
        !expect(precedence_tick.samples.size() == 1, "no shorthand animation")) {
        return 10;
    }

    ComputedStyle no_name;
    no_name.set("animation", "fade 1s linear");
    no_name.set("animation-name", "none");
    AnimationRuntime none_runtime;
    if (!expect(start_style_animations(none_runtime, 10, no_name, blocks).empty(),
                "none wins over shorthand")) {
        return 11;
    }

    ComputedStyle empty_name;
    empty_name.set("animation", "fade 1s linear");
    empty_name.set("animation-name", "");
    AnimationRuntime empty_name_runtime;
    if (!expect(start_style_animations(empty_name_runtime, 10, empty_name, blocks).empty(),
                "empty name wins over shorthand")) {
        return 12;
    }

    ComputedStyle invalid;
    invalid.set("animation", "spin 1s linear");
    invalid.set("animation-name", "fade");
    invalid.set("animation-duration", "-1s");
    AnimationRuntime invalid_runtime;
    if (!expect(start_style_animations(invalid_runtime, 11, invalid, blocks).empty(),
                "invalid longhand no fallback")) {
        return 13;
    }

    ComputedStyle bad_shorthand;
    bad_shorthand.set("animation", "fade bogus");
    AnimationRuntime bad_shorthand_runtime;
    if (!expect(start_style_animations(bad_shorthand_runtime, 12, bad_shorthand, blocks).empty(),
                "invalid shorthand")) {
        return 14;
    }

    ComputedStyle paused;
    paused.set("animation-name", "fade");
    paused.set("animation-duration", "1s");
    paused.set("animation-timing-function", "linear");
    paused.set("animation-play-state", "paused");
    AnimationRuntime paused_runtime;
    const auto paused_ids = start_style_animations(paused_runtime, 13, paused, blocks);
    if (!expect(paused_ids.size() == 1, "paused started") ||
        !expect(paused_runtime.active() == 1, "paused active")) {
        return 15;
    }
    const auto paused_hold = paused_runtime.tick(0.5);
    const std::string* paused_value = value_for(paused_hold, paused_ids.front());
    if (!expect(paused_value != nullptr && near(std::stof(*paused_value), 0.0f), "paused held") ||
        !expect(!paused_hold.keep_awake, "paused sleeps")) {
        return 15;
    }
    (void)paused_runtime.resume(paused_ids.front());
    const auto paused_resumed = paused_runtime.tick(0.5);
    const std::string* resumed_value = value_for(paused_resumed, paused_ids.front());
    if (!expect(resumed_value != nullptr && near(std::stof(*resumed_value), 0.5f),
                "paused resumes")) {
        return 15;
    }

    ComputedStyle missing;
    missing.set("animation-name", "missing");
    AnimationRuntime missing_runtime;
    if (!expect(start_style_animations(missing_runtime, 14, missing, blocks).empty(),
                "missing keyframes")) {
        return 16;
    }

    ComputedStyle empty_style;
    AnimationRuntime empty_runtime;
    if (!expect(start_style_animations(empty_runtime, 15, empty_style, blocks).empty(),
                "empty style")) {
        return 17;
    }

    std::cout << "animation_style: ok\n";
    return 0;
}
