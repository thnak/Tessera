// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_shorthand.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::ui::animation::AnimationParseResult;
using tessera::ui::animation::AnimationSpec;
using tessera::ui::animation::Direction;
using tessera::ui::animation::EasingKind;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::parse_animation_list;
using tessera::ui::animation::PlayState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_shorthand: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 0.0001;
}

bool is_default_ease(const AnimationSpec& spec) {
    return spec.easing.kind == EasingKind::CubicBezier && near(spec.easing.x1, 0.25) &&
           near(spec.easing.y1, 0.1) && near(spec.easing.x2, 0.25) && near(spec.easing.y2, 1.0);
}

bool invalid(const AnimationParseResult& result, const char* message) {
    return expect(!result.ok, message);
}

} // namespace

int main() {
    const auto full = parse_animation_list("slide-in 1s ease-in 250ms 2 alternate both paused");
    if (!expect(full.ok && full.animations.size() == 1, "full count")) {
        return 1;
    }
    const AnimationSpec& spec = full.animations[0];
    if (!expect(spec.name == "slide-in", "name") || !expect(near(spec.duration, 1.0), "duration") ||
        !expect(near(spec.delay, 0.25), "delay") ||
        !expect(near(spec.iterations, 2.0), "iterations") ||
        !expect(spec.direction == Direction::Alternate, "direction") ||
        !expect(spec.fill == FillMode::Both, "fill") ||
        !expect(spec.play_state == PlayState::Paused, "play state") ||
        !expect(spec.easing.kind == EasingKind::CubicBezier, "easing kind")) {
        return 2;
    }

    const auto defaults = parse_animation_list("fade");
    if (!expect(defaults.ok && defaults.animations.size() == 1, "defaults count")) {
        return 3;
    }
    const AnimationSpec& base = defaults.animations[0];
    if (!expect(base.name == "fade", "default name") ||
        !expect(near(base.duration, 0.0), "default duration") ||
        !expect(near(base.delay, 0.0), "default delay") ||
        !expect(near(base.iterations, 1.0), "default iterations") ||
        !expect(base.direction == Direction::Normal, "default direction") ||
        !expect(base.fill == FillMode::None, "default fill") ||
        !expect(base.play_state == PlayState::Running, "default play") ||
        !expect(is_default_ease(base), "default easing")) {
        return 4;
    }

    const auto list =
        parse_animation_list("fade 1s linear, spin 2s ease-out infinite reverse forwards");
    if (!expect(list.ok && list.animations.size() == 2, "list count") ||
        !expect(list.animations[0].name == "fade", "list first name") ||
        !expect(list.animations[0].easing.kind == EasingKind::Linear, "list first easing") ||
        !expect(list.animations[1].name == "spin", "list second name") ||
        !expect(std::isinf(list.animations[1].iterations), "infinite iterations") ||
        !expect(list.animations[1].direction == Direction::Reverse, "reverse") ||
        !expect(list.animations[1].fill == FillMode::Forwards, "forwards")) {
        return 5;
    }

    const auto ordered = parse_animation_list("2s paused slide linear");
    if (!expect(ordered.ok && ordered.animations.size() == 1, "ordered count") ||
        !expect(ordered.animations[0].name == "slide", "ordered name") ||
        !expect(near(ordered.animations[0].duration, 2.0), "ordered duration") ||
        !expect(ordered.animations[0].play_state == PlayState::Paused, "ordered play") ||
        !expect(ordered.animations[0].easing.kind == EasingKind::Linear, "ordered easing")) {
        return 6;
    }

    const auto delay = parse_animation_list("fade 1s -100ms");
    if (!expect(delay.ok && delay.animations.size() == 1, "negative delay count") ||
        !expect(near(delay.animations[0].delay, -0.1), "negative delay value")) {
        return 7;
    }

    const auto fractional = parse_animation_list("fade 1s .5 alternate-reverse backwards");
    if (!expect(fractional.ok && fractional.animations.size() == 1, "fractional count") ||
        !expect(near(fractional.animations[0].iterations, 0.5), "fractional iterations") ||
        !expect(fractional.animations[0].direction == Direction::AlternateReverse,
                "alternate reverse") ||
        !expect(fractional.animations[0].fill == FillMode::Backwards, "backwards")) {
        return 8;
    }

    const auto zero = parse_animation_list("fade 0s 0");
    if (!expect(zero.ok && zero.animations.size() == 1, "zero count") ||
        !expect(near(zero.animations[0].iterations, 0.0), "zero iterations")) {
        return 9;
    }

    const auto named_none = parse_animation_list("none");
    if (!expect(named_none.ok && named_none.animations.size() == 1, "none count") ||
        !expect(named_none.animations[0].name == "none", "none name") ||
        !expect(named_none.animations[0].fill == FillMode::None, "none fill")) {
        return 10;
    }

    const auto durations_only = parse_animation_list("1s 200ms");
    if (!expect(durations_only.ok && durations_only.animations.size() == 1, "times count") ||
        !expect(durations_only.animations[0].name == "none", "times name") ||
        !expect(near(durations_only.animations[0].duration, 1.0), "times duration") ||
        !expect(near(durations_only.animations[0].delay, 0.2), "times delay")) {
        return 11;
    }

    const auto steps = parse_animation_list("spin 1s steps(4, end)");
    if (!expect(steps.ok && steps.animations.size() == 1, "steps count") ||
        !expect(steps.animations[0].easing.kind == EasingKind::Steps, "steps easing") ||
        !expect(steps.animations[0].easing.steps == 4, "steps count value")) {
        return 12;
    }

    const auto bezier = parse_animation_list("spin 1s cubic-bezier(0.2, 0, 0.8, 1)");
    if (!expect(bezier.ok && bezier.animations.size() == 1, "bezier count") ||
        !expect(bezier.animations[0].easing.kind == EasingKind::CubicBezier, "bezier kind") ||
        !expect(near(bezier.animations[0].easing.x1, 0.2), "bezier x1")) {
        return 13;
    }

    if (!invalid(parse_animation_list("fade 1s,"), "empty slot") ||
        !invalid(parse_animation_list("fade 1s, , spin"), "middle empty slot") ||
        !invalid(parse_animation_list("-1s 200ms fade"), "negative duration") ||
        !invalid(parse_animation_list("fade 1s 2s 3s"), "too many times") ||
        !invalid(parse_animation_list("fade 1s 1e999"), "infinite number token") ||
        !invalid(parse_animation_list("fade spin"), "duplicate name") ||
        !invalid(parse_animation_list("fade both forwards"), "duplicate fill") ||
        !invalid(parse_animation_list("fade paused running"), "duplicate play") ||
        !invalid(parse_animation_list("fade normal reverse"), "duplicate direction") ||
        !invalid(parse_animation_list("fade 1s linear ease"), "duplicate easing") ||
        !invalid(parse_animation_list("fade 1s cubic-bezier(0, 0, 1"), "unbalanced easing") ||
        !invalid(parse_animation_list("fade 1s +spin"), "bad name")) {
        return 14;
    }

    std::cout << "animation_shorthand: ok\n";
    return 0;
}
