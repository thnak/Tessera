// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition_runtime.hpp"

#include <cmath>
#include <cstdio>
#include <utility>

namespace tessera::ui::animation {
namespace {

std::string format_value(double value) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

} // namespace

const TransitionSpec* find_transition(const std::vector<TransitionSpec>& transitions,
                                      std::string_view property) {
    const TransitionSpec* exact = nullptr;
    const TransitionSpec* fallback = nullptr;
    for (const TransitionSpec& transition : transitions) {
        if (transition.property == property) {
            exact = &transition;
        } else if (transition.property == "all") {
            fallback = &transition;
        }
    }
    return exact != nullptr ? exact : fallback;
}

AnimationId start_transition(AnimationRuntime& runtime, std::uint64_t target,
                             const TransitionSpec& transition, std::string property, double from,
                             double to, AnimationCallback on_complete) {
    if (!std::isfinite(from) || !std::isfinite(to) || transition.duration < 0.0f ||
        !std::isfinite(transition.duration) || !std::isfinite(transition.delay)) {
        return 0;
    }
    if (property.empty()) {
        property = transition.property;
    }

    style::KeyframeBlock keyframes;
    keyframes.name = "transition";
    keyframes.steps.push_back(
        style::KeyframeStep{0.0, {style::Declaration{property, format_value(from), false, 0}}});
    keyframes.steps.push_back(
        style::KeyframeStep{100.0, {style::Declaration{property, format_value(to), false, 0}}});

    KeyframeTiming timing;
    timing.duration = transition.duration;
    timing.delay = transition.delay;
    timing.iterations = 1.0;
    timing.fill = FillMode::Forwards;
    timing.easing = transition.easing;

    AnimationInput input;
    input.target = target;
    input.keyframes = std::move(keyframes);
    input.timing = timing;
    input.on_complete = std::move(on_complete);
    return runtime.start(std::move(input));
}

} // namespace tessera::ui::animation
