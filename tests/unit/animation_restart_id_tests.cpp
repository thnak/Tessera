// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::animation::AnimationCallback;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::FillMode;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_restart_id: " << message << "\n";
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

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, FillMode fill,
                  std::string property = {}, AnimationCallback callback = {}) {
    AnimationInput input;
    input.target = target;
    input.keyframes = opacity_block();
    input.timing.duration = 1.0;
    input.timing.fill = fill;
    input.property = std::move(property);
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
}

const std::string* value_for(const AnimationTickResult& result, AnimationId id,
                             std::string_view property) {
    for (const auto& sample : result.samples) {
        if (sample.id != id) {
            continue;
        }
        for (const auto& declaration : sample.declarations) {
            if (declaration.property == property) {
                return &declaration.value;
            }
        }
    }
    return nullptr;
}

} // namespace

int main() {
    AnimationRuntime active;
    const AnimationId active_id = start(active, 1, FillMode::None, "opacity");
    (void)active.tick(0.4);
    if (!expect(active.pause(active_id), "active paused") ||
        !expect(active.entries().front().paused, "paused set")) {
        return 1;
    }
    if (!expect(active.restart(active_id), "active restart") ||
        !expect(active.entries().front().elapsed == 0.0, "active elapsed reset") ||
        !expect(!active.entries().front().paused, "active unpaused")) {
        return 2;
    }
    const auto active_tick = active.tick(0.25);
    const std::string* active_value = value_for(active_tick, active_id, "opacity");
    if (!expect(active_value != nullptr && near(std::stof(*active_value), 0.25f),
                "active restarted at zero")) {
        return 3;
    }
    if (!expect(!active.restart(0), "zero id rejected") ||
        !expect(!active.restart(999), "unknown id rejected") ||
        !expect(active.active() == 1, "unknown leaves runtime")) {
        return 4;
    }

    AnimationRuntime fill;
    int callbacks = 0;
    const AnimationId fill_id = start(fill, 2, FillMode::Forwards, "opacity",
                                      [&](AnimationId id) { callbacks += id == fill_id ? 1 : 0; });
    (void)fill.tick(1.0);
    if (!expect(callbacks == 1, "fill callback first") ||
        !expect(fill.settled() == 1, "fill retained")) {
        return 5;
    }
    if (!expect(fill.restart(fill_id), "fill restart") ||
        !expect(fill.active() == 1, "fill active") ||
        !expect(fill.settled() == 0, "fill removed") ||
        !expect(fill.entries().front().id == fill_id, "fill id preserved") ||
        !expect(fill.entries().front().elapsed == 0.0, "fill elapsed reset") ||
        !expect(!fill.entries().front().paused, "fill unpaused") ||
        !expect(fill.entries().front().property == "opacity", "fill property preserved")) {
        return 6;
    }
    (void)fill.tick(0.5);
    const auto fill_tick = fill.tick(0.3);
    const std::string* fill_value = value_for(fill_tick, fill_id, "opacity");
    if (!expect(callbacks == 1, "fill callback not early") ||
        !expect(fill.settled() == 0, "fill active again") ||
        !expect(fill_value != nullptr && near(std::stof(*fill_value), 0.8f),
                "fill restarted from zero")) {
        return 7;
    }
    const auto fill_finish = fill.tick(0.3);
    const std::string* fill_final = value_for(fill_finish, fill_id, "opacity");
    if (!expect(callbacks == 2, "fill callback again") ||
        !expect(fill.settled() == 1, "fill retained again") ||
        !expect(fill_finish.finished.size() == 1 && fill_finish.finished.front() == fill_id,
                "fill finished id") ||
        !expect(fill_final != nullptr && near(std::stof(*fill_final), 1.0f), "fill final sample")) {
        return 8;
    }

    AnimationRuntime others;
    const AnimationId first = start(others, 3, FillMode::None);
    const AnimationId second = start(others, 3, FillMode::None);
    (void)others.tick(0.4);
    (void)others.pause(second);
    if (!expect(others.restart(first), "first restart") ||
        !expect(others.entries().size() == 2, "both active")) {
        return 9;
    }
    if (!expect(others.entries()[0].id == first && others.entries()[0].elapsed == 0.0,
                "first reset") ||
        !expect(!others.entries()[0].paused, "first unpaused") ||
        !expect(others.entries()[1].id == second && others.entries()[1].elapsed == 0.4,
                "second elapsed kept") ||
        !expect(others.entries()[1].paused, "second paused kept")) {
        return 10;
    }

    std::cout << "animation_restart_id: ok\n";
    return 0;
}
