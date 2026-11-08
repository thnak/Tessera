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
        std::cerr << "animation_pause: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

Declaration declaration(std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), false, 0};
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {declaration("opacity", "0")}});
    block.steps.push_back(KeyframeStep{100.0, {declaration("opacity", "1")}});
    return block;
}

KeyframeBlock width_block() {
    KeyframeBlock block;
    block.name = "grow";
    block.steps.push_back(KeyframeStep{0.0, {declaration("width", "10px")}});
    block.steps.push_back(KeyframeStep{100.0, {declaration("width", "20px")}});
    return block;
}

KeyframeTiming timing() {
    KeyframeTiming result;
    result.duration = 1.0;
    result.fill = FillMode::Forwards;
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, KeyframeBlock block,
                  tessera::ui::animation::AnimationCallback callback = {}) {
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(block);
    input.timing = timing();
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
    AnimationRuntime runtime;
    int completed = 0;
    const AnimationId fading =
        start(runtime, 7, opacity_block(), [&](AnimationId) { ++completed; });

    const auto quarter = runtime.tick(0.25);
    const std::string* quarter_value = value_for(quarter, fading, "opacity");
    if (!expect(quarter_value != nullptr && near(std::stof(*quarter_value), 0.25f),
                "quarter value") ||
        !expect(quarter.keep_awake, "running keeps awake")) {
        return 1;
    }

    if (!expect(runtime.pause(fading), "pause id") ||
        !expect(runtime.pause(fading), "pause idempotent") ||
        !expect(!runtime.pause(999), "unknown pause")) {
        return 2;
    }

    const auto held = runtime.tick(2.0);
    const std::string* held_value = value_for(held, fading, "opacity");
    if (!expect(held_value != nullptr && near(std::stof(*held_value), 0.25f), "held value") ||
        !expect(held.finished.empty(), "no completion while paused") ||
        !expect(!held.keep_awake, "paused releases wake") ||
        !expect(runtime.active() == 1, "entry remains") || !expect(completed == 0, "no callback")) {
        return 3;
    }

    const auto held_again = runtime.tick(5.0);
    const std::string* held_again_value = value_for(held_again, fading, "opacity");
    if (!expect(held_again_value != nullptr && near(std::stof(*held_again_value), 0.25f),
                "still held")) {
        return 4;
    }

    if (!expect(runtime.resume(fading), "resume id") ||
        !expect(runtime.resume(fading), "resume idempotent") ||
        !expect(!runtime.resume(999), "unknown resume")) {
        return 5;
    }

    const auto resumed = runtime.tick(0.25);
    const std::string* resumed_value = value_for(resumed, fading, "opacity");
    if (!expect(resumed_value != nullptr && near(std::stof(*resumed_value), 0.5f),
                "resumed value") ||
        !expect(resumed.keep_awake, "resumed keeps awake")) {
        return 6;
    }

    const auto done = runtime.tick(0.5);
    const std::string* final_value = value_for(done, fading, "opacity");
    if (!expect(done.finished.size() == 1 && done.finished[0] == fading, "finishes after resume") ||
        !expect(final_value != nullptr && near(std::stof(*final_value), 1.0f), "fill final") ||
        !expect(completed == 1, "callback after resume") ||
        !expect(!done.keep_awake, "wake released") || !expect(runtime.empty(), "settled") ||
        !expect(!runtime.pause(fading), "pause finished")) {
        return 7;
    }

    AnimationRuntime targets;
    int target_completions = 0;
    const auto callback = [&](AnimationId) { ++target_completions; };
    const AnimationId opacity_id = start(targets, 9, opacity_block(), callback);
    const AnimationId width_id = start(targets, 9, width_block(), callback);

    (void)targets.tick(0.25);
    if (!expect(targets.pause_target(9) == 2, "pause target") ||
        !expect(targets.pause_target(8) == 0, "unknown target") ||
        !expect(targets.pause_target(9) == 2, "pause target idempotent")) {
        return 8;
    }

    const auto target_held = targets.tick(3.0);
    const std::string* held_opacity = value_for(target_held, opacity_id, "opacity");
    const std::string* held_width = value_for(target_held, width_id, "width");
    if (!expect(held_opacity != nullptr && near(std::stof(*held_opacity), 0.25f),
                "target opacity held") ||
        !expect(held_width != nullptr && near(std::stof(*held_width), 12.5f),
                "target width held") ||
        !expect(target_held.finished.empty(), "target no finish") ||
        !expect(!target_held.keep_awake, "target wake released") ||
        !expect(target_completions == 0, "target no callback")) {
        return 9;
    }

    if (!expect(targets.resume_target(9) == 2, "resume target") ||
        !expect(targets.resume_target(8) == 0, "unknown resume target") ||
        !expect(targets.resume_target(9) == 2, "resume target idempotent")) {
        return 10;
    }

    const auto target_resumed = targets.tick(0.25);
    const std::string* resumed_opacity = value_for(target_resumed, opacity_id, "opacity");
    const std::string* resumed_width = value_for(target_resumed, width_id, "width");
    if (!expect(resumed_opacity != nullptr && near(std::stof(*resumed_opacity), 0.5f),
                "target opacity resumed") ||
        !expect(resumed_width != nullptr && near(std::stof(*resumed_width), 15.0f),
                "target width resumed") ||
        !expect(target_resumed.keep_awake, "target awake")) {
        return 11;
    }

    const auto target_done = targets.tick(0.5);
    if (!expect(target_done.finished.size() == 2, "target finishes") ||
        !expect(target_completions == 2, "target callbacks") ||
        !expect(!target_done.keep_awake, "target wake released after finish") ||
        !expect(targets.empty(), "target settled")) {
        return 12;
    }

    AnimationRuntime invalid;
    const AnimationId invalid_id = start(invalid, 10, opacity_block());
    (void)invalid.pause(invalid_id);
    const auto invalid_paused = invalid.tick(std::nan(""));
    if (!expect(invalid_paused.samples.empty(), "invalid dt no sample") ||
        !expect(!invalid_paused.keep_awake, "paused invalid dt sleeps") ||
        !expect(invalid.active() == 1, "paused invalid remains")) {
        return 13;
    }
    (void)invalid.resume(invalid_id);
    if (!expect(invalid.tick(std::nan("")).keep_awake, "invalid dt keeps awake") ||
        !expect(invalid.tick(-1.0).keep_awake, "negative dt keeps awake")) {
        return 14;
    }

    AnimationRuntime mixed;
    const AnimationId mixed_paused = start(mixed, 11, opacity_block());
    const AnimationId mixed_running = start(mixed, 12, width_block());
    (void)mixed.pause(mixed_paused);
    const auto mixed_tick = mixed.tick(0.5);
    if (!expect(mixed_tick.keep_awake, "mixed stays awake") ||
        !expect(value_for(mixed_tick, mixed_paused, "opacity") != nullptr, "paused sample") ||
        !expect(value_for(mixed_tick, mixed_running, "width") != nullptr, "running sample")) {
        return 15;
    }

    std::cout << "animation_pause: ok\n";
    return 0;
}
