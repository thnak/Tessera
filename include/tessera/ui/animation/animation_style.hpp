// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/animation_start.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <vector>

namespace tessera::ui::animation {

std::vector<AnimationId> start_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                const style::ComputedStyle& style,
                                                const std::vector<style::KeyframeBlock>& keyframes,
                                                AnimationCallback on_complete = {});

} // namespace tessera::ui::animation
