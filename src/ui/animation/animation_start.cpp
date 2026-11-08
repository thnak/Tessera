// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_start.hpp"

#include <cmath>
#include <utility>

namespace tessera::ui::animation {

AnimationId start_animation(AnimationRuntime& runtime, std::uint64_t target,
                            const AnimationSpec& spec, const style::KeyframeBlock& keyframes,
                            AnimationCallback on_complete) {
    if (spec.name.empty() || spec.name == "none" || keyframes.steps.empty() ||
        spec.play_state == PlayState::Paused || !std::isfinite(spec.duration) ||
        spec.duration < 0.0f || !std::isfinite(spec.delay) || std::isnan(spec.iterations) ||
        spec.iterations <= 0.0) {
        return 0;
    }
    if (!keyframes.name.empty() && keyframes.name != spec.name) {
        return 0;
    }

    KeyframeTiming timing;
    timing.duration = spec.duration;
    timing.delay = spec.delay;
    timing.iterations = spec.iterations;
    timing.direction = spec.direction;
    timing.fill = spec.fill;
    timing.easing = spec.easing;

    AnimationInput input;
    input.target = target;
    input.keyframes = keyframes;
    input.timing = timing;
    input.on_complete = std::move(on_complete);
    return runtime.start(std::move(input));
}

} // namespace tessera::ui::animation
