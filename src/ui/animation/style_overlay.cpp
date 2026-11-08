// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/style_overlay.hpp"

namespace tessera::ui::animation {

void apply_declarations(style::ComputedStyle& style,
                        const std::vector<style::Declaration>& declarations) {
    for (const style::Declaration& declaration : declarations) {
        if (!declaration.property.empty()) {
            style.set(declaration.property, declaration.value);
        }
    }
}

void apply_animation_sample(style::ComputedStyle& style, const AnimationSample& sample) {
    apply_declarations(style, sample.declarations);
}

void apply_animation_result(style::ComputedStyle& style, const AnimationTickResult& result,
                            std::uint64_t target) {
    for (const AnimationSample& sample : result.samples) {
        if (sample.target == target) {
            apply_animation_sample(style, sample);
        }
    }
}

AnimationOverlay collect_animation_declarations(const AnimationTickResult& result) {
    AnimationOverlay overlay;
    for (const AnimationSample& sample : result.samples) {
        std::vector<style::Declaration>& declarations = overlay[sample.target];
        for (const style::Declaration& declaration : sample.declarations) {
            if (!declaration.property.empty()) {
                declarations.push_back(declaration);
            }
        }
    }
    return overlay;
}

} // namespace tessera::ui::animation
