// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition_runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::find_transition;
using tessera::ui::animation::parse_transition_list;
using tessera::ui::animation::start_transition;
using tessera::ui::animation::TransitionSpec;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transition_runtime: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

float sample_value(const AnimationTickResult& result, AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id == id && !sample.declarations.empty()) {
            return std::stof(sample.declarations.front().value);
        }
    }
    return -1.0f;
}

} // namespace

int main() {
    const auto parsed = parse_transition_list("opacity 1s linear");
    if (!expect(parsed.ok && parsed.transitions.size() == 1, "parse spec")) {
        return 1;
    }

    AnimationRuntime runtime;
    int completed = 0;
    const AnimationId id = start_transition(runtime, 7, parsed.transitions[0], "", 0.0, 1.0,
                                            [&](AnimationId finished) {
                                                if (finished == id) {
                                                    ++completed;
                                                }
                                            });
    if (!expect(id != 0, "start id") || !expect(runtime.active() == 1, "active")) {
        return 2;
    }

    const auto quarter = runtime.tick(0.25);
    if (!expect(quarter.samples.size() == 1, "quarter sample") ||
        !expect(quarter.samples[0].target == 7, "target") ||
        !expect(quarter.samples[0].declarations.size() == 1, "declaration") ||
        !expect(quarter.samples[0].declarations[0].property == "opacity", "property") ||
        !expect(near(sample_value(quarter, id), 0.25f), "quarter value") ||
        !expect(quarter.keep_awake, "keeps awake")) {
        return 3;
    }

    const auto finished = runtime.tick(0.75);
    if (!expect(runtime.empty(), "settled") ||
        !expect(finished.finished.size() == 1 && finished.finished[0] == id, "finished id") ||
        !expect(near(sample_value(finished, id), 1.0f), "final sample") ||
        !expect(completed == 1, "completion callback") ||
        !expect(!finished.keep_awake, "released wake")) {
        return 4;
    }

    std::vector<TransitionSpec> transitions;
    transitions.push_back(TransitionSpec{"all", 1.0f, 0.0f, {}});
    transitions.push_back(TransitionSpec{"opacity", 2.0f, 0.0f, {}});
    const TransitionSpec* exact = find_transition(transitions, "opacity");
    const TransitionSpec* fallback = find_transition(transitions, "width");
    if (!expect(exact != nullptr && exact->duration == 2.0f, "exact wins") ||
        !expect(fallback != nullptr && fallback->property == "all", "all fallback")) {
        return 5;
    }

    std::vector<TransitionSpec> only_opacity;
    only_opacity.push_back(TransitionSpec{"opacity", 1.0f, 0.0f, {}});
    if (!expect(find_transition(only_opacity, "width") == nullptr, "missing transition")) {
        return 6;
    }

    TransitionSpec invalid = parsed.transitions[0];
    invalid.duration = -1.0f;
    AnimationRuntime invalid_runtime;
    if (!expect(start_transition(invalid_runtime, 1, invalid, "opacity", 0.0, 1.0) == 0,
                "negative duration rejected") ||
        !expect(start_transition(invalid_runtime, 1, parsed.transitions[0], "opacity", std::nan(""),
                                 1.0) == 0,
                "nan value rejected") ||
        !expect(invalid_runtime.empty(), "no invalid entries")) {
        return 7;
    }

    TransitionSpec zero;
    zero.property = "width";
    zero.duration = 0.0f;
    AnimationRuntime zero_runtime;
    const AnimationId zero_id = start_transition(zero_runtime, 2, zero, "", 10.0, 20.0);
    const auto zero_tick = zero_runtime.tick(0.0);
    if (!expect(zero_id != 0, "zero duration id") ||
        !expect(zero_runtime.empty(), "zero duration settled") ||
        !expect(!zero_tick.samples.empty(), "zero sample") ||
        !expect(near(sample_value(zero_tick, zero_id), 20.0f), "zero final value") ||
        !expect(zero_tick.samples[0].declarations[0].property == "width", "zero property")) {
        return 8;
    }

    std::cout << "transition_runtime: ok\n";
    return 0;
}
