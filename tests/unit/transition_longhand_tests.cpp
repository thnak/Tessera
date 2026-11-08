// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::ui::animation::EasingKind;
using tessera::ui::animation::parse_transition_longhands;
using tessera::ui::animation::TransitionParseResult;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "transition_longhand: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.0001f;
}

bool is_default_ease(const tessera::ui::animation::Easing& easing) {
    return easing.kind == EasingKind::CubicBezier && near(easing.x1, 0.25f) &&
           near(easing.y1, 0.1f) && near(easing.x2, 0.25f) && near(easing.y2, 1.0f);
}

bool invalid(const TransitionParseResult& result, const char* message) {
    return expect(!result.ok && result.transitions.empty(), message);
}

} // namespace

int main() {
    const auto cycled =
        parse_transition_longhands("opacity, width", "1s", "0s, 100ms", "linear, ease-in");
    if (!expect(cycled.ok && cycled.transitions.size() == 2, "two transitions") ||
        !expect(cycled.transitions[0].property == "opacity", "first property") ||
        !expect(near(cycled.transitions[0].duration, 1.0f), "first duration") ||
        !expect(near(cycled.transitions[0].delay, 0.0f), "first delay") ||
        !expect(cycled.transitions[0].easing.kind == EasingKind::Linear, "first easing") ||
        !expect(cycled.transitions[1].property == "width", "second property") ||
        !expect(near(cycled.transitions[1].duration, 1.0f), "cycled duration") ||
        !expect(near(cycled.transitions[1].delay, 0.1f), "second delay") ||
        !expect(cycled.transitions[1].easing.kind == EasingKind::CubicBezier, "second easing")) {
        return 1;
    }

    const auto defaults = parse_transition_longhands("opacity", "", "", "");
    if (!expect(defaults.ok && defaults.transitions.size() == 1, "defaults count") ||
        !expect(near(defaults.transitions[0].duration, 0.0f), "default duration") ||
        !expect(near(defaults.transitions[0].delay, 0.0f), "default delay") ||
        !expect(is_default_ease(defaults.transitions[0].easing), "default easing")) {
        return 2;
    }

    const auto all = parse_transition_longhands("all, opacity", "250ms, 1s", "", "linear");
    if (!expect(all.ok && all.transitions.size() == 2, "all count") ||
        !expect(all.transitions[0].property == "all", "all property") ||
        !expect(near(all.transitions[0].duration, 0.25f), "milliseconds") ||
        !expect(near(all.transitions[1].duration, 1.0f), "second duration") ||
        !expect(all.transitions[1].easing.kind == EasingKind::Linear, "cycled easing")) {
        return 3;
    }

    const auto negative_delay = parse_transition_longhands("opacity", "1s", "-100ms", "");
    if (!expect(negative_delay.ok && negative_delay.transitions.size() == 1, "negative delay") ||
        !expect(near(negative_delay.transitions[0].delay, -0.1f), "negative delay value")) {
        return 4;
    }

    const auto none = parse_transition_longhands("none", "1s", "", "");
    if (!expect(none.ok && none.transitions.empty(), "none disables")) {
        return 5;
    }

    const auto empty = parse_transition_longhands("", "1s", "", "");
    if (!expect(empty.ok && empty.transitions.empty(), "empty properties")) {
        return 6;
    }

    if (!invalid(parse_transition_longhands("opacity,", "", "", ""), "empty property slot") ||
        !invalid(parse_transition_longhands("1opacity", "", "", ""), "bad identifier") ||
        !invalid(parse_transition_longhands("opacity, none", "", "", ""), "mixed none") ||
        !invalid(parse_transition_longhands("opacity", "fast", "", ""), "bad duration") ||
        !invalid(parse_transition_longhands("opacity", "-1s", "", ""), "negative duration") ||
        !invalid(parse_transition_longhands("opacity", "1s", "", "bogus"), "bad easing") ||
        !invalid(parse_transition_longhands("opacity", "1s,", "", ""), "empty duration slot") ||
        !invalid(parse_transition_longhands("opacity", "1s", "0s", "linear,"),
                 "empty easing slot") ||
        !invalid(parse_transition_longhands("opacity", "1s", "", "cubic-bezier(0, 0, 1"),
                 "unbalanced easing")) {
        return 7;
    }

    std::cout << "transition_longhand: ok\n";
    return 0;
}
