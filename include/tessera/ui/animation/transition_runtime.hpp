// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/transition.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {
class ComputedStyle;
}

namespace tessera::ui::animation {

const TransitionSpec* find_transition(const std::vector<TransitionSpec>& transitions,
                                      std::string_view property);

AnimationId start_transition_values(AnimationRuntime& runtime, std::uint64_t target,
                                    const TransitionSpec& transition, std::string property,
                                    std::string from, std::string to,
                                    AnimationCallback on_complete = {});

AnimationId start_transition(AnimationRuntime& runtime, std::uint64_t target,
                             const TransitionSpec& transition, std::string property, double from,
                             double to, AnimationCallback on_complete = {});

std::vector<AnimationId> start_style_transitions(AnimationRuntime& runtime, std::uint64_t target,
                                                 const std::vector<TransitionSpec>& transitions,
                                                 const style::ComputedStyle& before,
                                                 const style::ComputedStyle& after,
                                                 AnimationCallback on_complete = {});

} // namespace tessera::ui::animation
