// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_restart: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock block(std::string name, std::string from, std::string to) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", std::move(from), false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", std::move(to), false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, KeyframeBlock keyframes,
                  FillMode fill = FillMode::None, double duration = 1.0,
                  tessera::ui::animation::AnimationCallback callback = {}) {
    KeyframeTiming timing;
    timing.duration = duration;
    timing.fill = fill;
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(keyframes);
    input.timing = timing;
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
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
    AnimationRuntime runtime;
    int completed = 0;
    const AnimationId id = start(runtime, 1, block("fade", "0", "1"), FillMode::None, 1.0,
                                 [&](AnimationId) { ++completed; });
    (void)runtime.tick(0.5);
    if (!expect(runtime.restart_target_name(1, "fade"), "restart active") ||
        !expect(runtime.entries().front().id == id, "same id") ||
        !expect(runtime.entries().front().elapsed == 0.0, "elapsed reset") ||
        !expect(!runtime.entries().front().paused, "unpaused")) {
        return 1;
    }
    const auto restarted_half = runtime.tick(0.5);
    const std::string* restarted_value = value_for(restarted_half, id);
    if (!expect(restarted_value != nullptr && near(std::stof(*restarted_value), 0.5f),
                "restarted halfway") ||
        !expect(restarted_half.keep_awake, "restart wakes") ||
        !expect(completed == 0, "no early callback")) {
        return 2;
    }
    const auto done = runtime.tick(0.5);
    if (!expect(done.finished.size() == 1 && completed == 1, "finishes once") ||
        !expect(!runtime.restart_target_name(1, "fade"), "finished no fill")) {
        return 3;
    }

    AnimationRuntime paused;
    const AnimationId paused_id = start(paused, 2, block("fade", "0", "1"), FillMode::None, 1.0);
    (void)paused.tick(0.5);
    (void)paused.pause(paused_id);
    if (!expect(paused.restart_target_name(2, "fade"), "restart paused") ||
        !expect(!paused.entries().front().paused, "restart unpauses") ||
        !expect(paused.entries().front().elapsed == 0.0, "paused elapsed reset")) {
        return 4;
    }
    const auto resumed = paused.tick(0.5);
    const std::string* resumed_value = value_for(resumed, paused_id);
    if (!expect(resumed_value != nullptr && near(std::stof(*resumed_value), 0.5f),
                "paused continues") ||
        !expect(resumed.keep_awake, "paused restart wakes")) {
        return 5;
    }

    AnimationRuntime fill;
    int fill_completed = 0;
    const AnimationId fill_id = start(fill, 3, block("fade", "0", "1"), FillMode::Forwards, 1.0,
                                      [&](AnimationId) { ++fill_completed; });
    (void)fill.tick(1.0);
    if (!expect(fill.settled() == 1 && fill_completed == 1, "fill settled") ||
        !expect(fill.restart_target_name(3, "fade"), "restart fill") ||
        !expect(fill.settled() == 0 && fill.active() == 1, "fill revived") ||
        !expect(fill.entries().front().id == fill_id, "fill id preserved") ||
        !expect(fill.entries().front().elapsed == 0.0, "fill elapsed reset")) {
        return 6;
    }
    const auto fill_quarter = fill.tick(0.25);
    const std::string* fill_value = value_for(fill_quarter, fill_id);
    if (!expect(fill_value != nullptr && near(std::stof(*fill_value), 0.25f), "fill replays") ||
        !expect(fill_quarter.keep_awake, "revived fill wakes")) {
        return 7;
    }
    const auto fill_done = fill.tick(0.75);
    if (!expect(fill_done.finished.size() == 1 && fill_completed == 2, "fill callback repeats") ||
        !expect(fill.settled() == 1, "fill settled again") ||
        !expect(fill.stop(fill_id), "stop fill again") ||
        !expect(fill.settled() == 0, "fill cleared")) {
        return 8;
    }

    AnimationRuntime isolated;
    const AnimationId fading = start(isolated, 4, block("fade", "0", "1"), FillMode::None, 2.0);
    const AnimationId spinning = start(isolated, 4, block("spin", "0", "1"), FillMode::None, 2.0);
    (void)isolated.pause(spinning);
    (void)isolated.tick(0.5);
    if (!expect(isolated.restart_target_name(4, "fade"), "one name restarts") ||
        !expect(isolated.active() == 2, "sibling remains")) {
        return 9;
    }
    const auto mixed = isolated.tick(0.5);
    const std::string* fading_value = value_for(mixed, fading);
    const std::string* spinning_value = value_for(mixed, spinning);
    if (!expect(fading_value != nullptr && near(std::stof(*fading_value), 0.25f),
                "fade restarted") ||
        !expect(spinning_value != nullptr && near(std::stof(*spinning_value), 0.0f),
                "paused sibling unchanged") ||
        !expect(mixed.keep_awake, "fade keeps awake")) {
        return 10;
    }
    if (!expect(!isolated.restart_target_name(4, ""), "empty name") ||
        !expect(!isolated.restart_target_name(4, "Fade"), "case sensitive") ||
        !expect(!isolated.restart_target_name(4, "missing"), "unknown name")) {
        return 11;
    }

    AnimationRuntime other;
    (void)start(other, 5, block("fade", "0", "1"), FillMode::Forwards);
    (void)other.tick(1.0);
    if (!expect(!other.restart_target_name(6, "fade"), "other target no match") ||
        !expect(other.settled() == 1, "other fill preserved") ||
        !expect(other.tick(0.0).samples.size() == 1, "other fill sample")) {
        return 12;
    }

    std::cout << "animation_restart: ok\n";
    return 0;
}
