// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/style_overlay.hpp"

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
using tessera::ui::animation::apply_animation_result;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_fill: " << message << "\n";
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
                  FillMode fill = FillMode::Forwards, double duration = 1.0,
                  std::string property = {},
                  tessera::ui::animation::AnimationCallback callback = {}) {
    KeyframeTiming timing;
    timing.duration = duration;
    timing.fill = fill;
    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(keyframes);
    input.timing = timing;
    input.property = std::move(property);
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
    const AnimationId fading = start(runtime, 1, block("fade", "0", "0.5"), FillMode::Forwards, 1.0,
                                     {}, [&](AnimationId) { ++completed; });
    const auto done = runtime.tick(1.0);
    const std::string* final_value = value_for(done, fading);
    if (!expect(final_value != nullptr && near(std::stof(*final_value), 0.5f), "fill final") ||
        !expect(done.finished.size() == 1 && done.finished[0] == fading, "finished once") ||
        !expect(!done.keep_awake, "wake released") || !expect(runtime.empty(), "no active") ||
        !expect(runtime.active() == 0, "active count") ||
        !expect(runtime.settled() == 1, "fill kept") || !expect(completed == 1, "callback once")) {
        return 1;
    }

    const auto persisted = runtime.tick(0.25);
    const std::string* persisted_value = value_for(persisted, fading);
    if (!expect(persisted.samples.size() == 1, "one persisted sample") ||
        !expect(persisted_value != nullptr && near(std::stof(*persisted_value), 0.5f),
                "persisted value") ||
        !expect(persisted.finished.empty(), "no repeat completion") ||
        !expect(!persisted.keep_awake, "persisted fill sleeps") ||
        !expect(completed == 1, "no repeat callback")) {
        return 2;
    }

    ComputedStyle style;
    style.set("opacity", "1");
    apply_animation_result(style, persisted, 1);
    const std::string* applied = style.find("opacity");
    if (!expect(applied != nullptr && near(std::stof(*applied), 0.5f), "overlay persists")) {
        return 3;
    }

    AnimationRuntime both;
    const AnimationId both_id = start(both, 2, block("blink", "0", "0.8"), FillMode::Both);
    (void)both.tick(1.0);
    if (!expect(both.settled() == 1, "both kept") ||
        !expect(value_for(both.tick(0.0), both_id) != nullptr, "both persists")) {
        return 4;
    }

    AnimationRuntime none;
    const AnimationId none_id = start(none, 3, block("once", "0", "1"), FillMode::None);
    (void)none.tick(1.0);
    if (!expect(none.settled() == 0, "none not kept") ||
        !expect(none.tick(0.0).samples.empty(), "none no fill") ||
        !expect(none.empty(), "none idle")) {
        return 5;
    }

    AnimationRuntime mixed;
    const AnimationId short_id = start(mixed, 4, block("short", "0", "1"), FillMode::Forwards, 1.0);
    const AnimationId long_id = start(mixed, 4, block("long", "0", "1"), FillMode::Forwards, 2.0);
    (void)short_id;
    const auto mixed_tick = mixed.tick(1.0);
    if (!expect(mixed_tick.finished.size() == 1, "short finished") ||
        !expect(mixed.active() == 1, "long active") ||
        !expect(mixed.settled() == 1, "short fill") ||
        !expect(mixed.stop_target(4) == 2, "stop target includes fill") ||
        !expect(mixed.empty() && mixed.settled() == 0, "stop target clears fill") ||
        !expect(mixed.tick(0.0).samples.empty(), "stopped target no sample")) {
        return 6;
    }

    AnimationRuntime stopped;
    const AnimationId stopped_id = start(stopped, 5, block("stop", "0", "1"));
    (void)stopped.tick(1.0);
    if (!expect(stopped.stop(stopped_id), "stop fill id") ||
        !expect(stopped.settled() == 0, "stop id clears fill") ||
        !expect(!stopped.stop(stopped_id), "stop fill twice")) {
        return 7;
    }
    (void)long_id;
    (void)none_id;

    AnimationRuntime property;
    const AnimationId property_id =
        start(property, 6, block("", "0", "1"), FillMode::Forwards, 1.0, "opacity");
    (void)property.tick(1.0);
    if (!expect(property.settled() == 1, "property fill kept") ||
        !expect(property.stop_target_property(6, "opacity") == 1, "stop property fill") ||
        !expect(property.settled() == 0, "property fill cleared")) {
        return 8;
    }
    (void)property_id;

    AnimationRuntime cleared;
    (void)start(cleared, 7, block("clear", "0", "1"));
    (void)cleared.tick(1.0);
    cleared.clear();
    if (!expect(cleared.settled() == 0, "clear fills") ||
        !expect(cleared.tick(0.0).samples.empty(), "clear no sample")) {
        return 9;
    }

    AnimationRuntime supersede;
    const AnimationId old_fill = start(supersede, 8, block("pulse", "0", "1"));
    (void)supersede.tick(1.0);
    if (!expect(supersede.settled() == 1, "old fill exists")) {
        return 10;
    }
    const AnimationId new_fill = start(supersede, 8, block("pulse", "0", "1"));
    if (!expect(supersede.settled() == 0, "same name supersedes") ||
        !expect(supersede.active() == 1, "new animation active")) {
        return 11;
    }
    (void)supersede.tick(1.0);
    if (!expect(supersede.settled() == 1, "new fill created") ||
        !expect(!supersede.stop(old_fill), "old fill gone") ||
        !expect(supersede.stop(new_fill), "new fill stops")) {
        return 12;
    }

    AnimationRuntime property_supersede;
    (void)start(property_supersede, 9, block("", "0", "1"), FillMode::Forwards, 1.0, "opacity");
    (void)property_supersede.tick(1.0);
    (void)start(property_supersede, 9, block("", "0", "1"), FillMode::Forwards, 1.0, "opacity");
    if (!expect(property_supersede.settled() == 0, "same property supersedes")) {
        return 13;
    }

    AnimationRuntime other_target;
    const AnimationId kept = start(other_target, 10, block("keep", "0", "1"));
    (void)other_target.tick(1.0);
    const AnimationId other = start(other_target, 11, block("keep", "0", "1"));
    if (!expect(other_target.settled() == 1, "other target fill kept") ||
        !expect(other_target.stop_target(11) == 1, "stop active other") ||
        !expect(other_target.settled() == 1, "kept fill remains") ||
        !expect(value_for(other_target.tick(0.0), kept) != nullptr, "kept fill sample")) {
        return 14;
    }
    (void)other;

    std::cout << "animation_fill: ok\n";
    return 0;
}
