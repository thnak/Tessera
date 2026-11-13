// SPDX-License-Identifier: MIT
//
// Registry entries that do not belong in the physical-longhand table of property_registry.cpp
// (stylesheet-v1 section 11, issue #250):
//
// * the `border` shorthand, which expands to the twelve physical border longhands;
// * the animation and transition names. Their grammars and expansion live in src/ui/animation/
//   (`animation_shorthand.cpp` for `animation`, `transition.cpp` for `transition`); the registry
//   only has to accept the names, so the cascade stores them and `@supports` answers true.
// * the name accessors the compatibility-matrix test walks.
//
// The animation and transition names all have an **empty initial value**: an animation or
// transition is absent until declared, which is what `parse_style_animations` and
// `parse_transition_list` already assume (they read the longhands only when `animation-name` /
// `transition-property` is present and otherwise fall back to the shorthand). Giving them a CSS
// initial value here would make every node look animated.

#include "tessera/ui/style/property_registry.hpp"

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {
namespace {

// The eight animation longhands and the `animation` shorthand, then the four transition longhands
// and the `transition` shorthand. None inherits (CSS).
constexpr const char* kAnimationProperties[] = {
    "animation-name",
    "animation-duration",
    "animation-timing-function",
    "animation-delay",
    "animation-iteration-count",
    "animation-direction",
    "animation-fill-mode",
    "animation-play-state",
    "animation",
    "transition-property",
    "transition-duration",
    "transition-timing-function",
    "transition-delay",
    "transition",
};

// `border` is `<width> || <style> || <color>` applied to all four sides, so it expands through the
// same BorderSide path as `border-inline-start`/`-end` (logical_properties.cpp) over twelve
// longhands instead of three.
constexpr const char* kBorderLonghands[] = {
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
};

} // namespace

void PropertyRegistry::register_extension_properties() {
    for (const char* name : kAnimationProperties) {
        properties_[name] = PropertyInfo{std::string(), false};
    }

    std::vector<std::string> border_longhands;
    border_longhands.reserve(std::size(kBorderLonghands));
    for (const char* name : kBorderLonghands) {
        border_longhands.emplace_back(name);
    }
    shorthands_["border"] = ShorthandInfo{std::move(border_longhands), ShorthandKind::BorderSide};
}

std::vector<std::string_view> PropertyRegistry::shorthand_names() const {
    std::vector<std::string_view> names;
    names.reserve(shorthands_.size());
    for (const auto& [name, info] : shorthands_) {
        (void)info;
        names.push_back(name);
    }
    return names;
}

} // namespace tessera::ui::style
