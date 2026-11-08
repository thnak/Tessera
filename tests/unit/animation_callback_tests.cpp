// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::animation::AnimationCallback;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_callback: " << message << "\n";
    }
    return condition;
}

KeyframeBlock block(std::string name) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, std::string name,
                  std::string property = {}, FillMode fill = FillMode::Forwards,
                  AnimationCallback callback = {}) {
    AnimationInput input;
    input.target = target;
    input.keyframes = block(std::move(name));
    input.timing.duration = 1.0;
    input.timing.fill = fill;
    input.property = std::move(property);
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
}

bool has_finished(const tessera::ui::animation::AnimationTickResult& result, AnimationId id) {
    for (const AnimationId finished : result.finished) {
        if (finished == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    AnimationRuntime cleared;
    AnimationId cleared_id = 0;
    bool clear_called = false;
    cleared_id = start(cleared, 7, "fade", {}, FillMode::Forwards, [&](AnimationId id) {
        clear_called = id == cleared_id;
        cleared.clear();
    });
    const auto clear_result = cleared.tick(1.0);
    if (!expect(clear_called, "clear callback id") ||
        !expect(has_finished(clear_result, cleared_id), "clear finished") ||
        !expect(cleared.active() == 0, "clear leaves no active") ||
        !expect(cleared.settled() == 0, "clear wins fill")) {
        return 1;
    }

    AnimationRuntime stopped;
    AnimationId stopped_id = 0;
    bool stop_called = false;
    std::size_t stop_removed = 0;
    stopped_id = start(stopped, 7, "fade", {}, FillMode::Forwards, [&](AnimationId id) {
        stop_called = id == stopped_id;
        stop_removed = stopped.stop_target_name(7, "fade");
    });
    const auto stop_result = stopped.tick(1.0);
    if (!expect(stop_called, "stop callback id") ||
        !expect(has_finished(stop_result, stopped_id), "stop finished") ||
        !expect(stop_removed == 1, "stop removes retained fill") ||
        !expect(stopped.settled() == 0, "stop wins fill")) {
        return 2;
    }

    AnimationRuntime replaced;
    AnimationId replaced_id = 0;
    AnimationId replacement_id = 0;
    bool replace_called = false;
    replaced_id = start(replaced, 8, "fade", {}, FillMode::Forwards, [&](AnimationId id) {
        replace_called = id == replaced_id;
        replacement_id = start(replaced, 8, "fade", {}, FillMode::None);
    });
    const auto replace_result = replaced.tick(1.0);
    if (!expect(replace_called, "replace callback id") ||
        !expect(has_finished(replace_result, replaced_id), "replace finished") ||
        !expect(replacement_id != 0 && replacement_id != replaced_id, "replacement id") ||
        !expect(replaced.active() == 1, "replacement active") ||
        !expect(replaced.settled() == 0, "name supersedes fill") ||
        !expect(replaced.entries().front().id == replacement_id, "replacement is active")) {
        return 3;
    }

    AnimationRuntime by_property;
    AnimationId property_id = 0;
    bool property_called = false;
    property_id = start(by_property, 9, "fade", "opacity", FillMode::Forwards, [&](AnimationId id) {
        property_called = id == property_id;
        (void)start(by_property, 9, "other", "opacity", FillMode::None);
    });
    const auto property_result = by_property.tick(1.0);
    if (!expect(property_called, "property callback id") ||
        !expect(has_finished(property_result, property_id), "property finished") ||
        !expect(by_property.active() == 1, "property replacement active") ||
        !expect(by_property.settled() == 0, "property supersedes fill")) {
        return 4;
    }

    AnimationRuntime ordered;
    std::vector<AnimationId> order;
    const AnimationId first = start(ordered, 10, "fade", {}, FillMode::Forwards,
                                    [&](AnimationId id) { order.push_back(id); });
    const AnimationId second = start(ordered, 11, "spin", {}, FillMode::Forwards,
                                     [&](AnimationId id) { order.push_back(id); });
    const auto ordered_result = ordered.tick(1.0);
    if (!expect(order.size() == 2 && order[0] == first && order[1] == second, "callback order") ||
        !expect(ordered.settled() == 2, "both fills retained") ||
        !expect(has_finished(ordered_result, first) && has_finished(ordered_result, second),
                "both finished")) {
        return 5;
    }

    std::cout << "animation_callback: ok\n";
    return 0;
}
