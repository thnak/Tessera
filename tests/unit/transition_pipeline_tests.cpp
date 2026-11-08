// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/style_overlay.hpp"
#include "tessera/ui/animation/transition_runtime.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/color.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::parse_transition_longhands;
using tessera::ui::animation::start_style_transitions;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transition_pipeline: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

float number(const ComputedStyle& style, const char* property, bool& found) {
    const std::string* value = style.find(property);
    found = value != nullptr;
    return found ? std::stof(*value) : -1.0f;
}

bool same_color(const ComputedStyle& style, const char* property, const char* expected) {
    const std::string* value = style.find(property);
    if (value == nullptr) {
        return false;
    }
    const auto actual = tessera::ui::style::parse_color(*value);
    const auto target = tessera::ui::style::parse_color(expected);
    return actual.ok && target.ok && near(actual.color.r, target.color.r) &&
           near(actual.color.g, target.color.g) && near(actual.color.b, target.color.b) &&
           near(actual.color.a, target.color.a);
}

} // namespace

int main() {
    const auto transitions =
        parse_transition_longhands("width, opacity, color", "1s, 2s", "0s", "linear");
    if (!expect(transitions.ok && transitions.transitions.size() == 3, "parse longhands")) {
        return 1;
    }

    ComputedStyle before;
    before.set("width", "10px");
    before.set("opacity", "1");
    before.set("color", "red");
    before.set("height", "5px");
    ComputedStyle after;
    after.set("width", "20px");
    after.set("opacity", "0.5");
    after.set("color", "blue");
    after.set("background-color", "red");
    after.set("margin", "0px");

    AnimationRuntime runtime;
    int completed = 0;
    const auto ids =
        start_style_transitions(runtime, 7, transitions.transitions, before, after,
                                [&](tessera::ui::animation::AnimationId) { ++completed; });
    if (!expect(ids.size() == 3, "three transitions") ||
        !expect(runtime.active() == 3, "three active")) {
        return 2;
    }

    const auto half = runtime.tick(0.5);
    ComputedStyle animated = after;
    apply_animation_result(animated, half, 7);
    bool found = false;
    const float width = number(animated, "width", found);
    if (!expect(found && near(width, 15.0f), "width halfway")) {
        return 3;
    }
    const float opacity = number(animated, "opacity", found);
    if (!expect(found && near(opacity, 0.875f), "opacity halfway")) {
        return 4;
    }
    const std::string* color = animated.find("color");
    if (!expect(color != nullptr && *color != "red" && *color != "blue", "color halfway") ||
        !expect(animated.find("background-color") != nullptr &&
                    *animated.find("background-color") == "red",
                "untouched property preserved") ||
        !expect(animated.find("margin") != nullptr && *animated.find("margin") == "0px",
                "new property untouched") ||
        !expect(animated.find("height") == nullptr, "removed property not added")) {
        return 5;
    }
    if (!expect(has_flag(half.dirty, Dirty::Layout), "layout dirty") ||
        !expect(has_flag(half.dirty, Dirty::Paint), "paint dirty") ||
        !expect(has_flag(half.dirty, Dirty::Render), "render dirty") ||
        !expect(half.keep_awake, "keeps awake")) {
        return 6;
    }

    const auto more = runtime.tick(0.5);
    apply_animation_result(animated, more, 7);
    const float width_done = number(animated, "width", found);
    const float opacity_more = number(animated, "opacity", found);
    if (!expect(more.finished.size() == 2, "two finish") ||
        !expect(found && near(width_done, 20.0f), "width final") ||
        !expect(found && near(opacity_more, 0.75f), "opacity continues") ||
        !expect(same_color(animated, "color", "blue"), "color final") ||
        !expect(*animated.find("background-color") == "red", "background preserved after finish")) {
        return 7;
    }

    const auto end = runtime.tick(1.0);
    ComputedStyle settled = after;
    apply_animation_result(settled, end, 7);
    const float final_opacity = number(settled, "opacity", found);
    if (!expect(end.finished.size() == 1, "last finishes") ||
        !expect(!end.keep_awake, "wake released") || !expect(runtime.empty(), "runtime settled") ||
        !expect(completed == 3, "three completions") ||
        !expect(found && near(final_opacity, 0.5f), "opacity final") ||
        !expect(*settled.find("width") == "20px", "settled width") ||
        !expect(same_color(settled, "color", "blue"), "settled color") ||
        !expect(*settled.find("margin") == "0px", "settled new property")) {
        return 8;
    }

    std::cout << "transition_pipeline: ok\n";
    return 0;
}
