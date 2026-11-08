// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/animation_shorthand.hpp"
#include "tessera/ui/animation/runtime.hpp"

#include <cstdint>

namespace tessera::ui::animation {

AnimationId start_animation(AnimationRuntime& runtime, std::uint64_t target,
                            const AnimationSpec& spec, const style::KeyframeBlock& keyframes,
                            AnimationCallback on_complete = {});

} // namespace tessera::ui::animation
