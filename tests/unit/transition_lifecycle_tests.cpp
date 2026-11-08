// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition_runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::parse_transition_list;
using tessera::ui::animation::start_transition_values;
using tessera::ui::animation::TransitionSpec;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transition_lifecycle: " << message << "\n";
    }
    return condition;
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
    const auto parsed = parse_transition_list("width 1s linear");
    if (!expect(parsed.ok && parsed.transitions.size() == 1, "parse spec")) {
        return 1;
    }
    const TransitionSpec& spec = parsed.transitions[0];

    AnimationRuntime runtime;
    int old_completions = 0;
    int new_completions = 0;
    const AnimationId old_id = start_transition_values(runtime, 7, spec, "width", "0px", "10px",
                                                       [&](AnimationId) { ++old_completions; });
    const auto half = runtime.tick(0.5);
    const std::string* half_width = property_value(half, "width");
    if (!expect(old_id != 0, "old started") ||
        !expect(runtime.entries().size() == 1 && runtime.entries()[0].property == "width",
                "property tagged") ||
        !expect(half_width != nullptr && *half_width == "5px", "old halfway")) {
        return 2;
    }

    const AnimationId new_id = start_transition_values(runtime, 7, spec, "width", "5px", "20px",
                                                       [&](AnimationId) { ++new_completions; });
    if (!expect(new_id != old_id, "new id") || !expect(runtime.active() == 1, "old superseded") ||
        !expect(runtime.entries()[0].id == new_id, "new remains")) {
        return 3;
    }

    const auto restarted = runtime.tick(0.5);
    const std::string* restarted_width = property_value(restarted, "width");
    if (!expect(restarted_width != nullptr && *restarted_width == "12.5px",
                "new starts from current value") ||
        !expect(old_completions == 0, "old callback suppressed") ||
        !expect(new_completions == 0, "new callback not early")) {
        return 4;
    }

    const auto finish = runtime.tick(0.5);
    if (!expect(finish.finished.size() == 1, "new finishes") ||
        !expect(new_completions == 1, "new callback fires") ||
        !expect(old_completions == 0, "old callback still suppressed") ||
        !expect(runtime.empty(), "superseded runtime settled")) {
        return 5;
    }

    const AnimationId other_target =
        start_transition_values(runtime, 8, spec, "width", "0px", "10px");
    const AnimationId other_property =
        start_transition_values(runtime, 7, spec, "opacity", "0", "1");
    if (!expect(other_target != 0 && other_property != 0, "other ids") ||
        !expect(runtime.active() == 2, "different target/property kept")) {
        return 6;
    }

    if (!expect(runtime.stop_target_property(7, "") == 0, "empty property ignored") ||
        !expect(runtime.active() == 2, "empty property keeps entries") ||
        !expect(runtime.stop_target_property(7, "width") == 0, "other target kept") ||
        !expect(runtime.stop_target_property(7, "opacity") == 1, "stop matching") ||
        !expect(runtime.active() == 1, "only one removed") ||
        !expect(runtime.stop_target_property(7, "opacity") == 0, "stop missing") ||
        !expect(runtime.stop_target_property(8, "width") == 1, "stop other target") ||
        !expect(runtime.empty(), "all stopped")) {
        return 7;
    }

    std::cout << "transition_lifecycle: ok\n";
    return 0;
}
