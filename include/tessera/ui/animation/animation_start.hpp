// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/animation_shorthand.hpp"
#include "tessera/ui/animation/runtime.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::ui::animation {

const style::KeyframeBlock* find_keyframes(const std::vector<style::KeyframeBlock>& keyframes,
                                           std::string_view name);

AnimationId start_animation(AnimationRuntime& runtime, std::uint64_t target,
                            const AnimationSpec& spec, const style::KeyframeBlock& keyframes,
                            AnimationCallback on_complete = {});

std::vector<AnimationId> start_animations(AnimationRuntime& runtime, std::uint64_t target,
                                          const std::vector<AnimationSpec>& specs,
                                          const std::vector<style::KeyframeBlock>& keyframes,
                                          AnimationCallback on_complete = {});

} // namespace tessera::ui::animation
