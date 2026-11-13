// SPDX-License-Identifier: MIT
//
// Computed style -> scroll runtime inputs (docs/specs/stylesheet-v1.md section 11): the properties
// that feed the scroll controller (`src/ui/scene/scroll.cpp`) and never trigger layout.
// `scrollbar-gutter` stays a layout input and is read directly by layout_style.cpp.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/scroll_style_sync.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string keyword(const ComputedStyle& style, const char* property) {
    const std::string* value = style.find(property);
    if (value == nullptr) {
        return {};
    }
    std::string result(trim(*value));
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

ScrollBehavior scroll_behavior_value(const ComputedStyle& style) {
    return keyword(style, "scroll-behavior") == "smooth" ? ScrollBehavior::Smooth
                                                         : ScrollBehavior::Auto;
}

OverscrollBehavior overscroll_behavior_value(const ComputedStyle& style) {
    const std::string value = keyword(style, "overscroll-behavior");
    if (value == "contain") {
        return OverscrollBehavior::Contain;
    }
    if (value == "none") {
        return OverscrollBehavior::None;
    }
    return OverscrollBehavior::Auto;
}

OverscrollEffect overscroll_effect_value(const ComputedStyle& style) {
    const std::string value = keyword(style, "overscroll-effect");
    if (value == "none") {
        return OverscrollEffect::None;
    }
    if (value == "glow") {
        return OverscrollEffect::Glow;
    }
    if (value == "stretch") {
        return OverscrollEffect::Stretch;
    }
    if (value == "bounce") {
        return OverscrollEffect::Bounce;
    }
    // The initial request (registry initial `platform`): the HAL profile resolves it.
    return OverscrollEffect::Platform;
}

} // namespace

bool apply_scroll_style(Node& node, const ComputedStyle& style) {
    bool changed = false;
    const ScrollBehavior behavior = scroll_behavior_value(style);
    if (behavior != node.scroll_behavior()) {
        node.set_scroll_behavior(behavior);
        changed = true;
    }
    const OverscrollBehavior overscroll = overscroll_behavior_value(style);
    if (overscroll != node.overscroll_behavior()) {
        node.set_overscroll_behavior(overscroll);
        changed = true;
    }
    const OverscrollEffect effect = overscroll_effect_value(style);
    if (effect != node.overscroll_effect()) {
        node.set_overscroll_effect(effect);
        changed = true;
    }
    return changed;
}

namespace {

void sync_node(Node& node, SceneStyleTree& tree, const CompiledStyleSheet& sheet,
               ScrollStyleSyncResult& result) {
    const ComputedStyle computed = tree.compute(sheet, node);
    ++result.nodes;
    if (apply_scroll_style(node, computed)) {
        ++result.changed;
    }
    for (const auto& child : node.children()) {
        sync_node(*child, tree, sheet, result);
    }
}

} // namespace

ScrollStyleSyncResult sync_scroll_style(Node& root, SceneStyleTree& tree,
                                        const CompiledStyleSheet& sheet) {
    ScrollStyleSyncResult result;
    sync_node(root, tree, sheet, result);
    return result;
}

} // namespace tessera::ui::style
