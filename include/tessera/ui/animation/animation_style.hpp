// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/animation_start.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <vector>

namespace tessera::ui::animation {

AnimationParseResult parse_style_animations(const style::ComputedStyle& style);

std::vector<AnimationId> start_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                const style::ComputedStyle& style,
                                                const std::vector<style::KeyframeBlock>& keyframes,
                                                AnimationCallback on_complete = {});

std::size_t apply_style_play_state(AnimationRuntime& runtime, std::uint64_t target,
                                   const style::ComputedStyle& style);

} // namespace tessera::ui::animation
