// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/easing.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::animation {

struct TransitionSpec {
    std::string property = "all";
    float duration = 0.0f;
    float delay = 0.0f;
    Easing easing = Easing{EasingKind::CubicBezier, 0.25f, 0.1f, 0.25f, 1.0f};
};

struct TransitionParseResult {
    std::vector<TransitionSpec> transitions;
    bool ok = true;
};

TransitionParseResult parse_transition_list(std::string_view text);

} // namespace tessera::ui::animation
