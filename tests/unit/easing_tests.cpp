// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/easing.hpp"
#include "tessera/ui/animation/keyframes.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::animation::ease;
using tessera::ui::animation::EasingKind;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::animation::parse_easing;
using tessera::ui::animation::sample_keyframes;
using tessera::ui::animation::StepPosition;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "easing: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float tolerance = 0.001f) {
    return std::abs(lhs - rhs) <= tolerance;
}

KeyframeBlock opacity_ramp() {
    KeyframeBlock block;
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

double sample_opacity(const KeyframeBlock& block, const KeyframeTiming& timing, double elapsed) {
    const auto declarations = sample_keyframes(block, timing, elapsed);
    for (const Declaration& declaration : declarations) {
        if (declaration.property == "opacity") {
            return std::stod(declaration.value);
        }
    }
    return -1.0;
}

} // namespace

int main() {
    if (!expect(near(ease({}, 0.25f), 0.25f), "linear") ||
        !expect(near(ease({}, -1.0f), 0.0f), "linear clamp low") ||
        !expect(near(ease({}, 2.0f), 1.0f), "linear clamp high") ||
        !expect(near(ease({}, std::nanf("")), 0.0f), "nan input")) {
        return 1;
    }

    const auto ease_in = parse_easing("ease-in");
    const auto ease_out = parse_easing("ease-out");
    if (!expect(ease_in.ok && ease_in.easing.kind == EasingKind::CubicBezier, "ease-in parse") ||
        !expect(ease_out.ok, "ease-out parse") ||
        !expect(ease(ease_in.easing, 0.5f) < 0.5f, "ease-in below linear") ||
        !expect(ease(ease_out.easing, 0.5f) > 0.5f, "ease-out above linear") ||
        !expect(near(ease(ease_in.easing, 0.0f), 0.0f), "ease-in start") ||
        !expect(near(ease(ease_in.easing, 1.0f), 1.0f), "ease-in end")) {
        return 2;
    }

    const auto identity = parse_easing("cubic-bezier(0, 0, 1, 1)");
    if (!expect(identity.ok, "identity parse") ||
        !expect(near(ease(identity.easing, 0.25f), 0.25f, 0.002f), "identity quarter") ||
        !expect(near(ease(identity.easing, 0.5f), 0.5f, 0.002f), "identity half")) {
        return 3;
    }

    const auto end_steps = parse_easing("steps(4)");
    if (!expect(end_steps.ok, "steps parse") ||
        !expect(end_steps.easing.step_position == StepPosition::JumpEnd, "steps end default") ||
        !expect(near(ease(end_steps.easing, 0.0f), 0.0f), "steps end zero") ||
        !expect(near(ease(end_steps.easing, 0.1f), 0.0f), "steps end first") ||
        !expect(near(ease(end_steps.easing, 0.25f), 0.25f), "steps end quarter") ||
        !expect(near(ease(end_steps.easing, 0.99f), 0.75f), "steps end near one") ||
        !expect(near(ease(end_steps.easing, 1.0f), 1.0f), "steps end one")) {
        return 4;
    }

    const auto start_steps = parse_easing("steps(4, start)");
    if (!expect(start_steps.ok, "steps start parse") ||
        !expect(start_steps.easing.step_position == StepPosition::JumpStart, "steps start") ||
        !expect(near(ease(start_steps.easing, 0.0f), 0.25f), "steps start zero") ||
        !expect(near(ease(start_steps.easing, 0.25f), 0.5f), "steps start quarter")) {
        return 5;
    }

    const auto none_steps = parse_easing("steps(4, jump-none)");
    const auto both_steps = parse_easing("steps(4, jump-both)");
    if (!expect(none_steps.ok && both_steps.ok, "jump variants parse") ||
        !expect(near(ease(none_steps.easing, 0.25f), 1.0f / 3.0f), "jump-none") ||
        !expect(near(ease(both_steps.easing, 0.0f), 0.2f), "jump-both start") ||
        !expect(near(ease(both_steps.easing, 0.5f), 0.6f), "jump-both half")) {
        return 6;
    }

    if (!expect(!parse_easing("bogus").ok, "unknown invalid") ||
        !expect(!parse_easing("cubic-bezier(0, 0, 1)").ok, "bezier arity invalid") ||
        !expect(!parse_easing("cubic-bezier(-1, 0, 1, 1)").ok, "bezier x invalid") ||
        !expect(!parse_easing("steps(0)").ok, "zero steps invalid") ||
        !expect(!parse_easing("steps(1, jump-none)").ok, "jump-none one invalid") ||
        !expect(!parse_easing("steps(2, sideways)").ok, "step position invalid")) {
        return 7;
    }

    const KeyframeBlock ramp = opacity_ramp();
    KeyframeTiming timing;
    timing.easing = parse_easing("ease-in").easing;
    const double eased = sample_opacity(ramp, timing, 0.25);
    if (!expect(eased > 0.0 && eased < 0.25, "sampler applies ease-in")) {
        return 8;
    }

    KeyframeTiming stepped;
    stepped.easing = parse_easing("steps(4)").easing;
    if (!expect(sample_opacity(ramp, stepped, 0.1) == 0.0, "sampler steps zero") ||
        !expect(sample_opacity(ramp, stepped, 0.3) == 0.25, "sampler steps quarter") ||
        !expect(sample_opacity(ramp, stepped, 0.6) == 0.5, "sampler steps half")) {
        return 9;
    }

    std::cout << "easing: ok\n";
    return 0;
}
