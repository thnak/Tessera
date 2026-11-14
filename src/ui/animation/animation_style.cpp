// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_style.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace tessera::ui::animation {
namespace {

std::string value(const style::ComputedStyle& style, const char* property) {
    const std::string* found = style.find(property);
    return found != nullptr ? *found : std::string();
}

} // namespace

AnimationParseResult parse_style_animations(const style::ComputedStyle& style) {
    if (const std::string* names = style.find("animation-name"); names != nullptr) {
        return parse_animation_longhands(
            *names, value(style, "animation-duration"), value(style, "animation-timing-function"),
            value(style, "animation-delay"), value(style, "animation-iteration-count"),
            value(style, "animation-direction"), value(style, "animation-fill-mode"),
            value(style, "animation-play-state"));
    }
    if (const std::string* shorthand = style.find("animation"); shorthand != nullptr) {
        return parse_animation_list(*shorthand);
    }
    return {};
}

TransitionParseResult parse_style_transitions(const style::ComputedStyle& style) {
    // The longhands win when `transition-property` is present, matching CSS and
    // `parse_style_animations`: a stylesheet that sets both forms keeps the longhands
    // authoritative.
    if (const std::string* properties = style.find("transition-property"); properties != nullptr) {
        return parse_transition_longhands(*properties, value(style, "transition-duration"),
                                          value(style, "transition-delay"),
                                          value(style, "transition-timing-function"));
    }
    if (const std::string* shorthand = style.find("transition"); shorthand != nullptr) {
        return parse_transition_list(*shorthand);
    }
    return {};
}

std::vector<AnimationId> start_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                const style::ComputedStyle& style,
                                                const std::vector<style::KeyframeBlock>& keyframes,
                                                AnimationCallback on_complete) {
    const AnimationParseResult parsed = parse_style_animations(style);
    if (!parsed.ok) {
        return {};
    }
    return start_animations(runtime, target, parsed.animations, keyframes, std::move(on_complete));
}

std::vector<AnimationId> ensure_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                                 const style::ComputedStyle& style,
                                                 const std::vector<style::KeyframeBlock>& keyframes,
                                                 AnimationCallback on_complete) {
    const AnimationParseResult parsed = parse_style_animations(style);
    if (!parsed.ok) {
        return {};
    }
    std::vector<AnimationId> ids;
    for (const AnimationSpec& spec : parsed.animations) {
        if (spec.name.empty() || spec.name == "none") {
            continue;
        }
        const AnimationNameState state = runtime.target_name_state(target, spec.name);
        if (state.active > 0) {
            if (spec.play_state == PlayState::Paused) {
                (void)runtime.pause_target_name(target, spec.name);
            } else {
                (void)runtime.resume_target_name(target, spec.name);
            }
            for (const AnimationEntry& entry : runtime.entries()) {
                if (entry.target == target && entry.keyframes.name == spec.name) {
                    ids.push_back(entry.id);
                    break;
                }
            }
            continue;
        }
        if (state.settled > 0) {
            continue;
        }
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

std::size_t apply_style_play_state(AnimationRuntime& runtime, std::uint64_t target,
                                   const style::ComputedStyle& style) {
    const AnimationParseResult parsed = parse_style_animations(style);
    if (!parsed.ok) {
        return 0;
    }
    std::size_t changed = 0;
    for (const AnimationEntry& entry : runtime.entries()) {
        if (entry.target != target || entry.keyframes.name.empty()) {
            continue;
        }
        const AnimationSpec* matched = nullptr;
        for (const AnimationSpec& spec : parsed.animations) {
            if (spec.name == entry.keyframes.name) {
                matched = &spec;
                break;
            }
        }
        if (matched == nullptr) {
            continue;
        }
        const bool paused = matched->play_state == PlayState::Paused;
        if (paused == entry.paused) {
            continue;
        }
        if (paused) {
            (void)runtime.pause(entry.id);
        } else {
            (void)runtime.resume(entry.id);
        }
        ++changed;
    }
    return changed;
}

StyleAnimationSync sync_style_animations(AnimationRuntime& runtime, std::uint64_t target,
                                         const style::ComputedStyle& style,
                                         const std::vector<style::KeyframeBlock>& keyframes,
                                         AnimationCallback on_complete) {
    StyleAnimationSync result;
    const AnimationParseResult parsed = parse_style_animations(style);
    if (!parsed.ok) {
        return result;
    }
    result.ok = true;

    std::vector<std::string> desired;
    for (const AnimationSpec& spec : parsed.animations) {
        if (!spec.name.empty() && spec.name != "none") {
            desired.push_back(spec.name);
        }
    }
    const auto wanted = [&](const std::string& name) {
        return std::find(desired.begin(), desired.end(), name) != desired.end();
    };

    std::vector<AnimationId> obsolete;
    for (const AnimationEntry& entry : runtime.entries()) {
        if (entry.target == target && !entry.keyframes.name.empty() &&
            !wanted(entry.keyframes.name)) {
            obsolete.push_back(entry.id);
        }
    }
    for (const AnimationId id : obsolete) {
        if (runtime.stop(id)) {
            ++result.stopped;
        }
    }

    obsolete.clear();
    for (const AnimationEntry& entry : runtime.settled_entries()) {
        if (entry.target == target && !entry.keyframes.name.empty() &&
            !wanted(entry.keyframes.name)) {
            obsolete.push_back(entry.id);
        }
    }
    for (const AnimationId id : obsolete) {
        if (runtime.stop(id)) {
            ++result.stopped;
        }
    }

    const std::size_t before = runtime.active();
    result.ids = ensure_style_animations(runtime, target, style, keyframes, on_complete);
    result.started = runtime.active() - before;
    return result;
}

} // namespace tessera::ui::animation
