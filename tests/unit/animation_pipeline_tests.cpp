// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_start.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/animation/style_overlay.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::collect_animation_declarations;
using tessera::ui::animation::parse_animation_list;
using tessera::ui::animation::start_animations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_pipeline: " << message << "\n";
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
    block.name = "grow";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"width", "10px", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"width", "20px", false, 0}}});
    return block;
}

float number(const ComputedStyle& style, const char* property, bool& found) {
    const std::string* value = style.find(property);
    found = value != nullptr;
    return found ? std::stof(*value) : -1.0f;
}

// A color fade is paint-only: it must never request layout work, unlike a border width change.
bool color_animation_is_paint_only() {
    using tessera::ui::animation::dirty_for_property;

    KeyframeBlock tint;
    tint.name = "tint";
    tint.steps.push_back(KeyframeStep{0.0, {Declaration{"border-top-color", "#000000", false, 0}}});
    tint.steps.push_back(
        KeyframeStep{100.0, {Declaration{"border-top-color", "#ffffff", false, 0}}});

    AnimationRuntime runtime;
    runtime.start(tessera::ui::animation::AnimationInput{9, tint, {}, {}, "", false});
    const auto half = runtime.tick(0.5);
    bool ok = expect(has_flag(half.dirty, Dirty::Paint) && has_flag(half.dirty, Dirty::Render),
                     "border color marks paint and render");
    ok = expect(!has_flag(half.dirty, Dirty::Layout), "border color never marks layout") && ok;
    ok = expect(has_flag(dirty_for_property("border-top-width"), Dirty::Layout),
                "border width still feeds layout") &&
         ok;
    ok = expect(!has_flag(dirty_for_property("background-color"), Dirty::Layout) &&
                    !has_flag(dirty_for_property("color"), Dirty::Layout) &&
                    !has_flag(dirty_for_property("border-left-color"), Dirty::Layout),
                "color-only properties are paint-only") &&
         ok;
    return ok;
}

} // namespace

int main() {
    const auto parsed = parse_animation_list("fade 1s linear forwards, grow 2s linear, missing 1s");
    if (!expect(parsed.ok && parsed.animations.size() == 3, "parse animations")) {
        return 1;
    }

    const std::vector<KeyframeBlock> blocks = {opacity_block(), width_block()};
    AnimationRuntime runtime;
    int completed = 0;
    const auto ids = start_animations(runtime, 7, parsed.animations, blocks,
                                      [&](tessera::ui::animation::AnimationId) { ++completed; });
    if (!expect(ids.size() == 2, "two resolved") || !expect(runtime.active() == 2, "two active")) {
        return 2;
    }

    ComputedStyle base;
    base.set("opacity", "1");
    base.set("width", "10px");
    base.set("height", "5px");
    base.set("color", "red");

    const auto half = runtime.tick(0.5);
    ComputedStyle animated = base;
    apply_animation_result(animated, half, 7);
    bool found = false;
    const float opacity = number(animated, "opacity", found);
    if (!expect(found && near(opacity, 0.5f), "opacity halfway")) {
        return 3;
    }
    const float width = number(animated, "width", found);
    if (!expect(found && near(width, 12.5f), "width quarter")) {
        return 4;
    }
    if (!expect(animated.find("height") != nullptr && *animated.find("height") == "5px",
                "height preserved") ||
        !expect(animated.find("color") != nullptr && *animated.find("color") == "red",
                "color preserved")) {
        return 5;
    }
    const auto overlay = collect_animation_declarations(half);
    const auto target_overlay = overlay.find(7);
    if (!expect(target_overlay != overlay.end() && target_overlay->second.size() == 2,
                "overlay declarations") ||
        !expect(has_flag(half.dirty, Dirty::Layout), "layout dirty") ||
        !expect(has_flag(half.dirty, Dirty::Paint), "paint dirty") ||
        !expect(has_flag(half.dirty, Dirty::Render), "render dirty") ||
        !expect(half.keep_awake, "keeps awake")) {
        return 6;
    }

    ComputedStyle wrong_target = base;
    apply_animation_result(wrong_target, half, 8);
    if (!expect(*wrong_target.find("opacity") == "1" && *wrong_target.find("width") == "10px",
                "target filtered")) {
        return 7;
    }

    const auto middle = runtime.tick(0.5);
    ComputedStyle fading = base;
    apply_animation_result(fading, middle, 7);
    const float middle_opacity = number(fading, "opacity", found);
    const float middle_width = number(fading, "width", found);
    if (!expect(middle.finished.size() == 1 && middle.finished[0] == ids[0], "fade finishes") ||
        !expect(found && near(middle_opacity, 1.0f), "forward fill final") ||
        !expect(found && near(middle_width, 15.0f), "width halfway") ||
        !expect(completed == 1, "one completion") || !expect(middle.keep_awake, "still awake")) {
        return 8;
    }

    const auto end = runtime.tick(1.0);
    ComputedStyle settled = base;
    apply_animation_result(settled, end, 7);
    if (!expect(end.finished.size() == 1 && end.finished[0] == ids[1], "grow finishes") ||
        !expect(end.samples.size() == 1, "retained fade fill sample") ||
        !expect(!end.keep_awake, "wake released") ||
        !expect(has_flag(end.dirty, Dirty::Paint), "fill paint dirty") ||
        !expect(!has_flag(end.dirty, Dirty::Layout), "fill not layout") ||
        !expect(runtime.empty(), "no active entries") ||
        !expect(runtime.settled() == 1, "fill retained") ||
        !expect(completed == 2, "two completions") ||
        !expect(*settled.find("opacity") == "1" && *settled.find("width") == "10px",
                "settled fill") ||
        !expect(*settled.find("height") == "5px", "settled height")) {
        return 9;
    }

    if (!color_animation_is_paint_only()) {
        return 10;
    }

    std::cout << "animation_pipeline: ok\n";
    return 0;
}
