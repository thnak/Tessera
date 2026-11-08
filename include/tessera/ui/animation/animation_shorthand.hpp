// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/easing.hpp"
#include "tessera/ui/animation/keyframes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::animation {

enum class PlayState {
    Running,
    Paused,
};

struct AnimationSpec {
    std::string name = "none";
    float duration = 0.0f;
    float delay = 0.0f;
    double iterations = 1.0;
    Direction direction = Direction::Normal;
    FillMode fill = FillMode::None;
    PlayState play_state = PlayState::Running;
    Easing easing = Easing{EasingKind::CubicBezier, 0.25f, 0.1f, 0.25f, 1.0f};
};

struct AnimationParseResult {
    std::vector<AnimationSpec> animations;
    bool ok = true;
};

AnimationParseResult parse_animation_list(std::string_view text);

AnimationParseResult
parse_animation_longhands(std::string_view names, std::string_view durations,
                          std::string_view timing_functions, std::string_view delays,
                          std::string_view iteration_counts, std::string_view directions,
                          std::string_view fill_modes, std::string_view play_states);

} // namespace tessera::ui::animation
