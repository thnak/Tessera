// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/style_overlay.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::animation::AnimationOverlay;
using tessera::ui::animation::AnimationSample;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::apply_animation_sample;
using tessera::ui::animation::apply_declarations;
using tessera::ui::animation::collect_animation_declarations;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_overlay: " << message << "\n";
    }
    return condition;
}

bool has(const ComputedStyle& style, const std::string& property, const std::string& value) {
    const std::string* found = style.find(property);
    return found != nullptr && *found == value;
}

Declaration declaration(std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), false, 0};
}

AnimationSample sample(std::uint64_t target, std::vector<Declaration> declarations) {
    AnimationSample result;
    result.target = target;
    result.declarations = std::move(declarations);
    return result;
}

} // namespace

int main() {
    ComputedStyle style;
    style.set("color", "red");
    style.set("width", "100px");

    AnimationTickResult result;
    result.samples.push_back(sample(7, {declaration("opacity", "0.5")}));
    result.samples.push_back(sample(8, {declaration("opacity", "1")}));
    result.samples.push_back(
        sample(7, {declaration("width", "200px"), declaration("", "ignored")}));
    result.samples.push_back(sample(7, {declaration("opacity", "0.75")}));

    apply_animation_result(style, result, 7);
    if (!expect(has(style, "opacity", "0.75"), "later sample wins") ||
        !expect(has(style, "width", "200px"), "multiple properties merge") ||
        !expect(has(style, "color", "red"), "unanimated property preserved") ||
        !expect(!style.contains(""), "empty property skipped")) {
        return 1;
    }

    ComputedStyle other;
    other.set("opacity", "0.25");
    apply_animation_result(other, result, 8);
    if (!expect(has(other, "opacity", "1"), "target filter") ||
        !expect(!other.contains("width"), "other target untouched")) {
        return 2;
    }

    ComputedStyle missing;
    missing.set("opacity", "0.25");
    apply_animation_result(missing, result, 99);
    if (!expect(has(missing, "opacity", "0.25"), "missing target no-op")) {
        return 3;
    }

    const AnimationOverlay overlay = collect_animation_declarations(result);
    if (!expect(overlay.size() == 2, "overlay targets") ||
        !expect(overlay.at(7).size() == 3, "overlay target 7 declarations") ||
        !expect(overlay.at(8).size() == 1, "overlay target 8 declarations") ||
        !expect(overlay.at(7)[2].value == "0.75", "overlay sample order")) {
        return 4;
    }

    ComputedStyle direct;
    direct.set("color", "blue");
    apply_declarations(direct, {declaration("height", "10px"), declaration("", "skip")});
    apply_animation_sample(
        direct, sample(1, {declaration("opacity", "0.5"), declaration("width", "20px")}));
    if (!expect(has(direct, "height", "10px") && has(direct, "opacity", "0.5") &&
                    has(direct, "width", "20px") && has(direct, "color", "blue"),
                "direct declarations")) {
        return 5;
    }

    std::cout << "animation_overlay: ok\n";
    return 0;
}
