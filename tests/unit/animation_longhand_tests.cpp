// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_shorthand.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using tessera::ui::animation::AnimationParseResult;
using tessera::ui::animation::Direction;
using tessera::ui::animation::EasingKind;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::parse_animation_longhands;
using tessera::ui::animation::PlayState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_longhand: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

AnimationParseResult parse(const char* names, const char* durations = "", const char* timing = "",
                           const char* delays = "", const char* iterations = "",
                           const char* directions = "", const char* fills = "",
                           const char* plays = "") {
    return parse_animation_longhands(names, durations, timing, delays, iterations, directions,
                                     fills, plays);
}

bool invalid(const char* names, const char* durations = "", const char* timing = "",
             const char* delays = "", const char* iterations = "", const char* directions = "",
             const char* fills = "", const char* plays = "") {
    const AnimationParseResult result =
        parse(names, durations, timing, delays, iterations, directions, fills, plays);
    return !result.ok && result.animations.empty();
}

} // namespace

int main() {
    const auto mixed = parse("fade, spin", "1s, 2s, 3s", "linear, ease-in", "100ms", "2",
                             "alternate", "forwards, both", "paused, running");
    if (!expect(mixed.ok && mixed.animations.size() == 2, "mixed count")) {
        return 1;
    }
    const auto& fade = mixed.animations[0];
    if (!expect(fade.name == "fade", "fade name") ||
        !expect(near(fade.duration, 1.0f), "fade duration") ||
        !expect(near(fade.delay, 0.1f), "fade delay") ||
        !expect(fade.easing.kind == EasingKind::Linear, "fade easing") ||
        !expect(fade.iterations == 2.0, "fade iterations") ||
        !expect(fade.direction == Direction::Alternate, "fade direction") ||
        !expect(fade.fill == FillMode::Forwards, "fade fill") ||
        !expect(fade.play_state == PlayState::Paused, "fade paused")) {
        return 2;
    }
    const auto& spin = mixed.animations[1];
    if (!expect(spin.name == "spin", "spin name") ||
        !expect(near(spin.duration, 2.0f), "spin duration") ||
        !expect(near(spin.delay, 0.1f), "spin delay") ||
        !expect(spin.easing.kind == EasingKind::CubicBezier, "spin easing") ||
        !expect(spin.iterations == 2.0, "spin iterations") ||
        !expect(spin.direction == Direction::Alternate, "spin direction") ||
        !expect(spin.fill == FillMode::Both, "spin fill") ||
        !expect(spin.play_state == PlayState::Running, "spin running")) {
        return 3;
    }

    const auto defaults = parse("only");
    if (!expect(defaults.ok && defaults.animations.size() == 1, "defaults count")) {
        return 4;
    }
    const auto& plain = defaults.animations[0];
    if (!expect(plain.name == "only", "default name") ||
        !expect(plain.duration == 0.0f, "default duration") ||
        !expect(plain.delay == 0.0f, "default delay") ||
        !expect(plain.easing.kind == EasingKind::CubicBezier, "default easing") ||
        !expect(plain.iterations == 1.0, "default iterations") ||
        !expect(plain.direction == Direction::Normal, "default direction") ||
        !expect(plain.fill == FillMode::None, "default fill") ||
        !expect(plain.play_state == PlayState::Running, "default play")) {
        return 5;
    }

    const auto timed =
        parse("a, b", "1.5s, 500ms", "cubic-bezier(0.1, 0.2, 0.3, 1), steps(4, end)", "-250ms");
    if (!expect(timed.ok && timed.animations.size() == 2, "timed count") ||
        !expect(near(timed.animations[0].duration, 1.5f), "seconds") ||
        !expect(near(timed.animations[1].duration, 0.5f), "milliseconds") ||
        !expect(near(timed.animations[0].delay, -0.25f), "negative delay cycle") ||
        !expect(near(timed.animations[1].delay, -0.25f), "negative delay reused") ||
        !expect(timed.animations[0].easing.kind == EasingKind::CubicBezier, "bezier") ||
        !expect(timed.animations[1].easing.kind == EasingKind::Steps, "steps") ||
        !expect(timed.animations[1].easing.steps == 4, "steps count")) {
        return 6;
    }

    const auto repeated = parse("a, b", "", "", "", "infinite, 0");
    if (!expect(repeated.ok && repeated.animations.size() == 2, "iteration count") ||
        !expect(std::isinf(repeated.animations[0].iterations), "infinite") ||
        !expect(repeated.animations[1].iterations == 0.0, "zero iterations")) {
        return 7;
    }

    const auto directions = parse("a, b, c", "", "", "", "", "reverse, alternate-reverse, normal");
    if (!expect(directions.ok && directions.animations.size() == 3, "directions count") ||
        !expect(directions.animations[0].direction == Direction::Reverse, "reverse") ||
        !expect(directions.animations[1].direction == Direction::AlternateReverse,
                "alternate reverse") ||
        !expect(directions.animations[2].direction == Direction::Normal, "normal")) {
        return 8;
    }

    const auto fills = parse("a, b, c", "", "", "", "", "", "backwards, both, none");
    if (!expect(fills.ok && fills.animations.size() == 3, "fills count") ||
        !expect(fills.animations[0].fill == FillMode::Backwards, "backwards") ||
        !expect(fills.animations[1].fill == FillMode::Both, "both") ||
        !expect(fills.animations[2].fill == FillMode::None, "fill none")) {
        return 9;
    }

    if (!expect(parse("").ok && parse("").animations.empty(), "empty names") ||
        !expect(parse("none").ok && parse("none").animations.empty(), "none names") ||
        !expect(parse("None").ok && parse("None").animations.empty(), "none case")) {
        return 10;
    }
    const auto mixed_none = parse("none, spin");
    if (!expect(mixed_none.ok && mixed_none.animations.size() == 2, "none item count") ||
        !expect(mixed_none.animations[0].name == "none", "none item") ||
        !expect(mixed_none.animations[1].name == "spin", "named item")) {
        return 11;
    }

    if (!expect(invalid("a", "-1s"), "negative duration") ||
        !expect(invalid("a", "1"), "unitless duration") ||
        !expect(invalid("a", "1s,"), "trailing duration comma") ||
        !expect(invalid("a,,b"), "empty name slot") ||
        !expect(invalid("a,"), "trailing name comma") ||
        !expect(invalid("1bad"), "bad identifier") ||
        !expect(invalid("a, b", "", "linear, bogus"), "bad easing") ||
        !expect(invalid("a", "", "linear,"), "empty easing slot") ||
        !expect(invalid("a", "", "", "", "-2"), "negative iterations") ||
        !expect(invalid("a", "", "", "", "many"), "bad iterations") ||
        !expect(invalid("a", "", "", "", "", "sideways"), "bad direction") ||
        !expect(invalid("a", "", "", "", "", "", "always"), "bad fill") ||
        !expect(invalid("a", "", "", "", "", "", "", "slow"), "bad play state") ||
        !expect(invalid("a, b", "1s, nope"), "partial duration failure")) {
        return 12;
    }

    std::cout << "animation_longhand: ok\n";
    return 0;
}
