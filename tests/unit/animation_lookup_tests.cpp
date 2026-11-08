// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_start.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::find_keyframes;
using tessera::ui::animation::parse_animation_list;
using tessera::ui::animation::start_animations;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_lookup: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock block(std::string name, std::string to) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", std::move(to), false, 0}}});
    return result;
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
    const std::vector<KeyframeBlock> blocks = {block("fade", "1"), block("spin", "1"),
                                               block("rest", "1"), block("zero", "1")};
    if (!expect(find_keyframes(blocks, "spin") != nullptr, "exact lookup") ||
        !expect(find_keyframes(blocks, "Spin") == nullptr, "case sensitive") ||
        !expect(find_keyframes(blocks, "missing") == nullptr, "missing lookup") ||
        !expect(find_keyframes(blocks, "") == nullptr, "empty lookup")) {
        return 1;
    }

    const std::vector<KeyframeBlock> duplicates = {block("fade", "1"), block("fade", "2")};
    const KeyframeBlock* last = find_keyframes(duplicates, "fade");
    if (!expect(last != nullptr && last->steps.back().declarations.front().value == "2",
                "last duplicate wins")) {
        return 2;
    }

    const auto parsed = parse_animation_list(
        "fade 1s linear forwards, spin 2s linear, missing 1s, rest 1s paused, zero 1s 0");
    if (!expect(parsed.ok && parsed.animations.size() == 5, "batch parse")) {
        return 3;
    }

    AnimationRuntime runtime;
    int completed = 0;
    const auto ids =
        start_animations(runtime, 7, parsed.animations, blocks, [&](AnimationId) { ++completed; });
    if (!expect(ids.size() == 2, "two started") || !expect(runtime.active() == 2, "two active")) {
        return 4;
    }

    const auto half = runtime.tick(0.5);
    const std::string* fade_value = value_for(half, ids[0]);
    const std::string* spin_value = value_for(half, ids[1]);
    if (!expect(fade_value != nullptr && near(std::stof(*fade_value), 0.5f), "fade halfway") ||
        !expect(spin_value != nullptr && near(std::stof(*spin_value), 0.25f), "spin halfway")) {
        return 5;
    }

    const auto fade_done = runtime.tick(0.5);
    if (!expect(fade_done.finished.size() == 1 && fade_done.finished[0] == ids[0],
                "fade finishes") ||
        !expect(completed == 1, "one completion") ||
        !expect(runtime.active() == 1, "spin remains")) {
        return 6;
    }

    const auto spin_done = runtime.tick(1.0);
    if (!expect(spin_done.finished.size() == 1 && spin_done.finished[0] == ids[1],
                "spin finishes") ||
        !expect(completed == 2, "two completions") || !expect(runtime.empty(), "settled")) {
        return 7;
    }

    AnimationRuntime duplicate_runtime;
    const auto duplicate_ids =
        start_animations(duplicate_runtime, 8, parsed.animations, duplicates);
    if (!expect(duplicate_ids.size() == 1, "duplicate spec starts fade only")) {
        return 8;
    }
    const auto duplicate_done = duplicate_runtime.tick(1.0);
    const std::string* duplicate_value = value(duplicate_done);
    if (!expect(duplicate_value != nullptr && near(std::stof(*duplicate_value), 2.0f),
                "last block used")) {
        return 9;
    }

    AnimationRuntime empty_runtime;
    if (!expect(start_animations(empty_runtime, 9, {}, blocks).empty(), "no specs") ||
        !expect(start_animations(empty_runtime, 9, parsed.animations, {}).empty(),
                "no keyframes") ||
        !expect(empty_runtime.empty(), "nothing started")) {
        return 10;
    }

    std::cout << "animation_lookup: ok\n";
    return 0;
}
