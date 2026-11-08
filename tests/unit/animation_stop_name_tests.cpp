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
        std::cerr << "animation_stop_name: " << message << "\n";
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
    const auto callback = [&](AnimationId) { ++completed; };
    const AnimationId stopped =
        start(runtime, 7, block("fade", "0", "1"), FillMode::None, 1.0, callback);
    const AnimationId spin =
        start(runtime, 7, block("spin", "0", "1"), FillMode::None, 1.0, callback);
    const AnimationId other_target =
        start(runtime, 8, block("fade", "0", "1"), FillMode::None, 1.0, callback);

    if (!expect(runtime.stop_target_name(7, "fade") == 1, "stop name count") ||
        !expect(runtime.active() == 2, "siblings remain") ||
        !expect(runtime.entries()[0].id == spin, "spin preserved") ||
        !expect(runtime.entries()[1].id == other_target, "other target preserved") ||
        !expect(runtime.stop_target_name(7, "Fade") == 0, "case sensitive") ||
        !expect(runtime.stop_target_name(7, "") == 0, "empty name") ||
        !expect(runtime.stop_target_name(7, "missing") == 0, "unknown name") ||
        !expect(!runtime.stop(stopped), "stopped id gone")) {
        return 1;
    }

    const auto done = runtime.tick(1.0);
    if (!expect(done.finished.size() == 2, "two siblings finish") ||
        !expect(completed == 2, "stopped callback skipped") ||
        !expect(!done.keep_awake, "wake released") || !expect(runtime.empty(), "settled") ||
        !expect(runtime.settled() == 0, "no fills")) {
        return 2;
    }

    AnimationRuntime paused;
    const AnimationId paused_id = start(paused, 9, block("fade", "0", "1"));
    (void)paused.pause(paused_id);
    if (!expect(paused.stop_target_name(9, "fade") == 1, "paused removed") ||
        !expect(paused.empty(), "paused settled") ||
        !expect(paused.tick(0.0).samples.empty(), "no sample")) {
        return 3;
    }

    AnimationRuntime fill;
    const AnimationId fill_id = start(fill, 10, block("fade", "0", "1"), FillMode::Forwards);
    (void)fill.tick(1.0);
    if (!expect(fill.settled() == 1, "fill exists") ||
        !expect(fill.stop_target_name(10, "fade") == 1, "fill stopped") ||
        !expect(fill.settled() == 0, "fill cleared") ||
        !expect(fill.tick(0.0).samples.empty(), "fill no sample")) {
        return 4;
    }
    (void)fill_id;

    AnimationRuntime mixed;
    (void)start(mixed, 11, block("fade", "0", "1"), FillMode::Forwards, 1.0);
    (void)start(mixed, 11, block("fade", "0", "1"), FillMode::Forwards, 2.0);
    (void)mixed.tick(1.0);
    if (!expect(mixed.settled() == 1 && mixed.active() == 1, "active plus fill") ||
        !expect(mixed.stop_target_name(11, "fade") == 2, "active and fill count") ||
        !expect(mixed.empty() && mixed.settled() == 0, "both cleared")) {
        return 5;
    }

    AnimationRuntime targets;
    const AnimationId kept = start(targets, 12, block("spin", "0", "1"));
    const AnimationId removed = start(targets, 13, block("spin", "0", "1"));
    if (!expect(targets.stop_target_name(12, "spin") == 1, "target-specific removal") ||
        !expect(targets.active() == 1, "other target remains") ||
        !expect(!targets.stop(kept), "removed target id gone") ||
        !expect(targets.stop(removed), "other target stops")) {
        return 6;
    }

    AnimationRuntime running;
    const AnimationId running_id = start(running, 14, block("spin", "0", "1"));
    (void)running.tick(0.25);
    if (!expect(!running.stop_target_name(14, "fade"), "different name no-op") ||
        !expect(running.active() == 1, "different name remains")) {
        return 7;
    }
    const auto resumed = running.tick(0.25);
    const auto value = value_for(resumed, running_id);
    if (!expect(value != nullptr && near(std::stof(*value), 0.5f), "remains running")) {
        return 8;
    }

    std::cout << "animation_stop_name: ok\n";
    return 0;
}
