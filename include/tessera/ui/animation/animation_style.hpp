// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/animation_start.hpp"
#include "tessera/ui/animation/transition.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <cstdint>
#include <vector>

namespace tessera::ui::animation {

struct StyleAnimationSync {
    bool ok = false;
    std::size_t started = 0;
    std::size_t stopped = 0;
    std::vector<AnimationId> ids;
};

AnimationParseResult parse_style_animations(const style::ComputedStyle& style);

// Transition counterpart of `parse_style_animations` (issue #271): reads the four longhands when
// `transition-property` is present and otherwise falls back to the `transition` shorthand, reusing
// `parse_transition_longhands`/`parse_transition_list` so there is one grammar. Both sets have an
// empty registry initial value (property_registry_extra.cpp), so an undeclared transition parses to
// an empty, `ok` list rather than to `all 0s`.
TransitionParseResult parse_style_transitions(const style::ComputedStyle& style);

std::vector<AnimationId> start_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                const style::ComputedStyle& style,
                                                const std::vector<style::KeyframeBlock>& keyframes,
                                                AnimationCallback on_complete = {});

std::vector<AnimationId> ensure_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                 const style::ComputedStyle& style,
                                                 const std::vector<style::KeyframeBlock>& keyframes,
                                                 AnimationCallback on_complete = {});

std::size_t apply_style_play_state(AnimationRuntime& runtime, std::uint64_t target,
                                   const style::ComputedStyle& style);

StyleAnimationSync sync_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                         const style::ComputedStyle& style,
                                         const std::vector<style::KeyframeBlock>& keyframes,
                                         AnimationCallback on_complete = {});

} // namespace tessera::ui::animation
