// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::ui::animation::Easing;
using tessera::ui::animation::EasingKind;
using tessera::ui::animation::parse_transition_list;
using tessera::ui::animation::StepPosition;
using tessera::ui::animation::TransitionParseResult;
using tessera::ui::animation::TransitionSpec;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transition: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.0001f;
}

bool is_ease(const Easing& easing) {
    return easing.kind == EasingKind::CubicBezier && near(easing.x1, 0.25f) &&
           near(easing.y1, 0.1f) && near(easing.x2, 0.25f) && near(easing.y2, 1.0f);
}

} // namespace

int main() {
    const TransitionParseResult list = parse_transition_list(
        "opacity 200ms ease-in 50ms, transform 1s cubic-bezier(0.1, 0.2, 0.3, 1.0), "
        "width 0.3s steps(4, end) -100ms");
    if (!expect(list.ok, "valid list") || !expect(list.transitions.size() == 3, "list size")) {
        return 1;
    }
    const TransitionSpec& opacity = list.transitions[0];
    if (!expect(opacity.property == "opacity", "opacity property") ||
        !expect(near(opacity.duration, 0.2f), "opacity duration") ||
        !expect(near(opacity.delay, 0.05f), "opacity delay") ||
        !expect(opacity.easing.kind == EasingKind::CubicBezier && near(opacity.easing.x1, 0.42f),
                "opacity easing")) {
        return 2;
    }
    const TransitionSpec& transform = list.transitions[1];
    if (!expect(transform.property == "transform", "transform property") ||
        !expect(near(transform.duration, 1.0f), "transform duration") ||
        !expect(near(transform.delay, 0.0f), "transform delay") ||
        !expect(transform.easing.kind == EasingKind::CubicBezier &&
                    near(transform.easing.x1, 0.1f) && near(transform.easing.y1, 0.2f),
                "transform bezier")) {
        return 3;
    }
    const TransitionSpec& width = list.transitions[2];
    if (!expect(width.property == "width", "width property") ||
        !expect(near(width.duration, 0.3f), "width duration") ||
        !expect(near(width.delay, -0.1f), "width delay") ||
        !expect(width.easing.kind == EasingKind::Steps && width.easing.steps == 4 &&
                    width.easing.step_position == StepPosition::JumpEnd,
                "width steps")) {
        return 4;
    }

    const TransitionParseResult defaults = parse_transition_list("ease 1s");
    if (!expect(defaults.ok && defaults.transitions.size() == 1, "default spec") ||
        !expect(defaults.transitions[0].property == "all", "default property") ||
        !expect(near(defaults.transitions[0].duration, 1.0f), "default duration") ||
        !expect(is_ease(defaults.transitions[0].easing), "explicit ease")) {
        return 5;
    }

    const TransitionParseResult implicit = parse_transition_list("all 500ms");
    if (!expect(implicit.ok && implicit.transitions.size() == 1, "implicit spec") ||
        !expect(near(implicit.transitions[0].duration, 0.5f), "implicit duration") ||
        !expect(is_ease(implicit.transitions[0].easing), "implicit ease") ||
        !expect(near(implicit.transitions[0].delay, 0.0f), "implicit delay")) {
        return 6;
    }

    const TransitionParseResult no_animations = parse_transition_list(" none ");
    const TransitionParseResult blank = parse_transition_list("  ");
    if (!expect(no_animations.ok && no_animations.transitions.empty(), "none") ||
        !expect(blank.ok && blank.transitions.empty(), "blank")) {
        return 7;
    }

    const TransitionParseResult negative = parse_transition_list("opacity -1s");
    const TransitionParseResult two_easings = parse_transition_list("opacity 1s ease-in ease-out");
    const TransitionParseResult extra_property = parse_transition_list("opacity 1s garbage extra");
    const TransitionParseResult trailing = parse_transition_list("opacity 1s,");
    const TransitionParseResult unbalanced =
        parse_transition_list("opacity 1s cubic-bezier(0.1, 0.2, 0.3, 1");
    if (!expect(!negative.ok && negative.transitions.empty(), "negative duration") ||
        !expect(!two_easings.ok && two_easings.transitions.empty(), "two easings") ||
        !expect(!extra_property.ok && extra_property.transitions.empty(), "extra property") ||
        !expect(!trailing.ok && trailing.transitions.size() == 1, "trailing comma") ||
        !expect(!unbalanced.ok && unbalanced.transitions.empty(), "unbalanced function")) {
        return 8;
    }

    const TransitionParseResult mixed_none = parse_transition_list("none, opacity 1s");
    if (!expect(!mixed_none.ok && mixed_none.transitions.size() == 1, "none mixed with spec")) {
        return 9;
    }

    std::cout << "transition: ok\n";
    return 0;
}
