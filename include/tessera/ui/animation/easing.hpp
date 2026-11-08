// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace tessera::ui::animation {

enum class EasingKind {
    Linear,
    CubicBezier,
    Steps,
};

enum class StepPosition {
    JumpStart,
    JumpEnd,
    JumpNone,
    JumpBoth,
};

struct Easing {
    EasingKind kind = EasingKind::Linear;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 1.0f;
    float y2 = 1.0f;
    int steps = 1;
    StepPosition step_position = StepPosition::JumpEnd;
};

struct EasingParseResult {
    Easing easing;
    bool ok = true;
};

EasingParseResult parse_easing(std::string_view text);
float ease(const Easing& easing, float t);

} // namespace tessera::ui::animation
