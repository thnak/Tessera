// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace tessera::ui::animation {

using AnimationOverlay = std::map<std::uint64_t, std::vector<style::Declaration>>;

void apply_declarations(style::ComputedStyle& style,
                        const std::vector<style::Declaration>& declarations);
void apply_animation_sample(style::ComputedStyle& style, const AnimationSample& sample);
void apply_animation_result(style::ComputedStyle& style, const AnimationTickResult& result,
                            std::uint64_t target);

AnimationOverlay collect_animation_declarations(const AnimationTickResult& result);

} // namespace tessera::ui::animation
