// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_start.hpp"

#include <cmath>
#include <utility>

namespace tessera::ui::animation {

const style::KeyframeBlock* find_keyframes(const std::vector<style::KeyframeBlock>& keyframes,
                                           std::string_view name) {
    if (name.empty()) {
        return nullptr;
    }
    const style::KeyframeBlock* found = nullptr;
    for (const style::KeyframeBlock& block : keyframes) {
        if (block.name == name) {
            found = &block;
        }
    }
    return found;
}

AnimationId start_animation(AnimationRuntime& runtime, std::uint64_t target,
                            const AnimationSpec& spec, const style::KeyframeBlock& keyframes,
                            AnimationCallback on_complete) {
    if (spec.name.empty() || spec.name == "none" || keyframes.steps.empty() ||
        !std::isfinite(spec.duration) || spec.duration < 0.0f || !std::isfinite(spec.delay) ||
        std::isnan(spec.iterations) || spec.iterations <= 0.0) {
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
    input.paused = spec.play_state == PlayState::Paused;
    return runtime.start(std::move(input));
}

std::vector<AnimationId> start_animations(AnimationRuntime& runtime, std::uint64_t target,
                                          const std::vector<AnimationSpec>& specs,
                                          const std::vector<style::KeyframeBlock>& keyframes,
                                          AnimationCallback on_complete) {
    std::vector<AnimationId> ids;
    for (const AnimationSpec& spec : specs) {
        const style::KeyframeBlock* block = find_keyframes(keyframes, spec.name);
        if (block == nullptr) {
            continue;
        }
        const AnimationId id = start_animation(runtime, target, spec, *block, on_complete);
        if (id != 0) {
            ids.push_back(id);
        }
    }
    return ids;
}

} // namespace tessera::ui::animation
