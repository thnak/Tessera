// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/keyframes.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::animation::Direction;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::animation::sample_keyframes;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 0.001;
}

Declaration declaration(std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), false, 0};
}

KeyframeStep step(double offset, std::string property, std::string value) {
    return KeyframeStep{offset, {declaration(std::move(property), std::move(value))}};
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "pulse";
    block.steps.push_back(step(0.0, "opacity", "0"));
    block.steps.push_back(step(50.0, "opacity", "1"));
    block.steps.push_back(step(100.0, "opacity", "0"));
    return block;
}

const std::string* value(const std::vector<Declaration>& declarations, std::string_view property) {
    for (const Declaration& declaration : declarations) {
        if (declaration.property == property) {
            return &declaration.value;
        }
    }
    return nullptr;
}

bool opacity_is(const std::vector<Declaration>& declarations, double expected) {
    const std::string* found = value(declarations, "opacity");
    if (found == nullptr) {
        std::cerr << "  opacity missing, expected " << expected << "\n";
        return false;
    }
    const bool matched = near(std::stod(*found), expected);
    if (!matched) {
        std::cerr << "  opacity expected " << expected << " got " << *found << "\n";
    }
    return matched;
}

} // namespace

int main() {
    const KeyframeBlock pulse = opacity_block();
    KeyframeTiming timing;
    timing.duration = 1.0;

    if (!expect(opacity_is(sample_keyframes(pulse, timing, 0.25), 0.5), "linear quarter") ||
        !expect(opacity_is(sample_keyframes(pulse, timing, 0.5), 1.0), "linear half") ||
        !expect(opacity_is(sample_keyframes(pulse, timing, 0.75), 0.5), "linear three quarter") ||
        !expect(opacity_is(sample_keyframes(pulse, timing, 0.0), 0.0), "linear start")) {
        return 1;
    }

    KeyframeTiming end_timing = timing;
    end_timing.fill = FillMode::Forwards;
    if (!expect(opacity_is(sample_keyframes(pulse, end_timing, 1.0), 0.0), "linear end")) {
        return 2;
    }

    KeyframeBlock lengths;
    lengths.steps.push_back(step(0.0, "width", "10px"));
    lengths.steps.push_back(step(100.0, "width", "20px"));
    const auto length_sample = sample_keyframes(lengths, timing, 0.5);
    if (!expect(value(length_sample, "width") != nullptr, "unit value present") ||
        !expect(*value(length_sample, "width") == "15px", "unit interpolation")) {
        return 2;
    }

    KeyframeBlock discrete;
    discrete.steps.push_back(step(0.0, "font-family", "serif"));
    discrete.steps.push_back(step(100.0, "font-family", "sans-serif"));
    if (!expect(*value(sample_keyframes(discrete, timing, 0.25), "font-family") == "serif",
                "discrete first half") ||
        !expect(*value(sample_keyframes(discrete, timing, 0.75), "font-family") == "sans-serif",
                "discrete second half")) {
        return 3;
    }

    KeyframeBlock colors;
    colors.steps.push_back(step(0.0, "color", "red"));
    colors.steps.push_back(step(100.0, "color", "blue"));
    if (!expect(*value(sample_keyframes(colors, timing, 0.5), "color") == "rgb(128, 0, 128)",
                "color interpolation")) {
        return 4;
    }

    KeyframeTiming delayed;
    delayed.duration = 1.0;
    delayed.delay = 1.0;
    delayed.fill = FillMode::None;
    if (!expect(sample_keyframes(pulse, delayed, 0.5).empty(), "delay without backwards fill")) {
        return 4;
    }

    delayed.fill = FillMode::Backwards;
    if (!expect(opacity_is(sample_keyframes(pulse, delayed, 0.5), 0.0), "backwards fill start")) {
        return 5;
    }

    delayed.fill = FillMode::Forwards;
    if (!expect(opacity_is(sample_keyframes(pulse, delayed, 2.0), 0.0), "forwards fill end")) {
        return 6;
    }

    KeyframeTiming reverse;
    reverse.duration = 1.0;
    reverse.direction = Direction::Reverse;
    if (!expect(opacity_is(sample_keyframes(pulse, reverse, 0.25), 0.5), "reverse")) {
        return 7;
    }

    KeyframeTiming alternate;
    alternate.duration = 1.0;
    alternate.iterations = 2.0;
    alternate.direction = Direction::Alternate;
    if (!expect(opacity_is(sample_keyframes(pulse, alternate, 0.25), 0.5),
                "alternate first iteration") ||
        !expect(opacity_is(sample_keyframes(pulse, alternate, 1.25), 0.5),
                "alternate second iteration")) {
        return 8;
    }

    alternate.fill = FillMode::Forwards;
    if (!expect(opacity_is(sample_keyframes(pulse, alternate, 2.0), 0.0),
                "alternate forwards fill")) {
        return 9;
    }

    KeyframeTiming alternate_reverse;
    alternate_reverse.duration = 1.0;
    alternate_reverse.iterations = 2.0;
    alternate_reverse.direction = Direction::AlternateReverse;
    if (!expect(opacity_is(sample_keyframes(pulse, alternate_reverse, 0.25), 0.5),
                "alternate reverse first") ||
        !expect(opacity_is(sample_keyframes(pulse, alternate_reverse, 1.25), 0.5),
                "alternate reverse second")) {
        return 10;
    }

    KeyframeTiming infinite;
    infinite.duration = 1.0;
    infinite.iterations = std::numeric_limits<double>::infinity();
    infinite.direction = Direction::Alternate;
    if (!expect(opacity_is(sample_keyframes(pulse, infinite, 100.25), 0.5),
                "infinite iterations")) {
        return 11;
    }

    KeyframeTiming empty_timing = timing;
    empty_timing.iterations = 0.0;
    if (!expect(sample_keyframes(pulse, empty_timing, 0.5).empty(), "zero iterations") ||
        !expect(sample_keyframes(KeyframeBlock{}, timing, 0.5).empty(), "empty keyframes") ||
        !expect(sample_keyframes(pulse, timing, std::nan("")).empty(), "nan elapsed")) {
        return 12;
    }

    std::cout << "animation: ok\n";
    return 0;
}
