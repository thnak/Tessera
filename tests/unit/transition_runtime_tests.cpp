// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition_runtime.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::find_transition;
using tessera::ui::animation::parse_transition_list;
using tessera::ui::animation::start_style_transitions;
using tessera::ui::animation::start_transition;
using tessera::ui::animation::start_transition_values;
using tessera::ui::animation::TransitionSpec;
using tessera::ui::style::ComputedStyle;

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

const std::string* property_value(const AnimationTickResult& result, std::string_view property) {
    for (const auto& sample : result.samples) {
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

    AnimationRuntime values_runtime;
    if (!expect(start_transition_values(values_runtime, 1, parsed.transitions[0], "width", "",
                                        "10px") == 0,
                "empty value rejected") ||
        !expect(values_runtime.empty(), "no empty-value entries")) {
        return 9;
    }

    ComputedStyle before;
    before.set("width", "10px");
    before.set("opacity", "1");
    before.set("background-color", "red");
    before.set("color", "red");
    before.set("height", "5px");
    ComputedStyle after;
    after.set("width", "20px");
    after.set("opacity", "0.5");
    after.set("background-color", "red");
    after.set("color", "blue");
    after.set("margin", "0px");

    const auto style_transitions = parse_transition_list("opacity 2s linear, all 1s linear");
    if (!expect(style_transitions.ok, "style transitions parse")) {
        return 10;
    }

    AnimationRuntime style_runtime;
    int style_completed = 0;
    const auto style_ids =
        start_style_transitions(style_runtime, 9, style_transitions.transitions, before, after,
                                [&](AnimationId) { ++style_completed; });
    if (!expect(style_ids.size() == 3, "changed properties only") ||
        !expect(style_runtime.active() == 3, "three active") ||
        !expect(find_transition(style_transitions.transitions, "margin") != nullptr,
                "all matches new property") ||
        !expect(find_transition(style_transitions.transitions, "height") != nullptr,
                "all matches removed property")) {
        return 11;
    }

    const auto style_half = style_runtime.tick(0.5);
    const std::string* width_value = property_value(style_half, "width");
    const std::string* opacity_value = property_value(style_half, "opacity");
    const std::string* color_value = property_value(style_half, "color");
    if (!expect(width_value != nullptr && *width_value == "15px", "unit preserved") ||
        !expect(opacity_value != nullptr && near(std::stof(*opacity_value), 0.875f),
                "exact transition wins") ||
        !expect(color_value != nullptr && *color_value != "red" && *color_value != "blue",
                "color interpolated") ||
        !expect(style_completed == 0, "style not complete")) {
        return 12;
    }

    const auto style_more = style_runtime.tick(0.5);
    if (!expect(style_more.finished.size() == 2, "width and color finish") ||
        !expect(style_completed == 2, "two completions") ||
        !expect(style_runtime.active() == 1, "opacity remains")) {
        return 13;
    }

    const auto style_end = style_runtime.tick(1.0);
    if (!expect(style_end.finished.size() == 1, "opacity finishes") ||
        !expect(style_completed == 3, "all completions") ||
        !expect(style_runtime.empty(), "style settled")) {
        return 14;
    }

    std::cout << "transition_runtime: ok\n";
    return 0;
}
