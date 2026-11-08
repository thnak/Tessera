// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"

#include <string>
#include <utility>

namespace tessera::ui::animation {
namespace {

std::string value(const style::ComputedStyle& style, const char* property) {
    const std::string* found = style.find(property);
    return found != nullptr ? *found : std::string();
}

} // namespace

std::vector<AnimationId> start_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                const style::ComputedStyle& style,
                                                const std::vector<style::KeyframeBlock>& keyframes,
                                                AnimationCallback on_complete) {
    if (const std::string* names = style.find("animation-name"); names != nullptr) {
        const AnimationParseResult parsed = parse_animation_longhands(
            *names, value(style, "animation-duration"), value(style, "animation-timing-function"),
            value(style, "animation-delay"), value(style, "animation-iteration-count"),
            value(style, "animation-direction"), value(style, "animation-fill-mode"),
            value(style, "animation-play-state"));
        if (!parsed.ok) {
            return {};
        }
        return start_animations(runtime, target, parsed.animations, keyframes,
                                std::move(on_complete));
    }

    if (const std::string* shorthand = style.find("animation"); shorthand != nullptr) {
        const AnimationParseResult parsed = parse_animation_list(*shorthand);
        if (!parsed.ok) {
            return {};
        }
        return start_animations(runtime, target, parsed.animations, keyframes,
                                std::move(on_complete));
    }
    return {};
}

} // namespace tessera::ui::animation
