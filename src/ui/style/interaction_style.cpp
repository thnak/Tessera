// SPDX-License-Identifier: MIT
//
// Computed style -> scene graph, for the two interaction properties the scene itself consumes:
// `pointer-events` (hit testing) and `user-select` (text selection). The property side
// (z-index, transform, shadow reach, glass) stays in `paint::sync_scene_style`.

#include "tessera/ui/style/interaction_style.hpp"

#include <cctype>
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

bool equals_lower(std::string_view text, std::string_view keyword) {
    if (text.size() != keyword.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != keyword[i]) {
            return false;
        }
    }
    return true;
}

// Only an explicit/inherited `none` disables pointer input. `auto`, an unsupported value that
// stayed in the AST (reported at parse time) and a missing property all keep the node hit-testable,
// which is the CSS fallback.
bool pointer_events_enabled(const std::string* value) {
    if (value == nullptr) {
        return true;
    }
    return !equals_lower(trim(*value), "none");
}

// `auto` and any unsupported form (`all`, `contain`) resolve to `Auto`, so V1 falls back to the
// default selectable behaviour instead of blocking a gesture for a value it did not implement.
UserSelect user_select_value(const std::string* value) {
    if (value == nullptr) {
        return UserSelect::Auto;
    }
    const std::string_view normalized = trim(*value);
    if (equals_lower(normalized, "none")) {
        return UserSelect::None;
    }
    if (equals_lower(normalized, "text")) {
        return UserSelect::Text;
    }
    return UserSelect::Auto;
}

void sync_node(Node& node, InteractionStyleFn compute, void* context,
               InteractionStyleSyncResult& result) {
    if (!node.visible()) {
        // Invisible subtrees are neither painted nor hit tested, so their policy is irrelevant and
        // walking them would only cost style computations.
        return;
    }
    const ComputedStyle style = compute(context, node);
    ++result.nodes;

    const bool pointer_events = pointer_events_enabled(style.find("pointer-events"));
    if (pointer_events != node.pointer_events()) {
        node.set_pointer_events(pointer_events);
        ++result.changed_pointer_events;
    }
    const UserSelect user_select = user_select_value(style.find("user-select"));
    if (user_select != node.user_select()) {
        node.set_user_select(user_select);
        ++result.changed_user_select;
    }

    for (const auto& child : node.children()) {
        sync_node(*child, compute, context, result);
    }
}

} // namespace

InteractionStyleSyncResult sync_interaction_style(Node& root, InteractionStyleFn compute,
                                                  void* context) {
    InteractionStyleSyncResult result;
    if (compute == nullptr) {
        return result;
    }
    sync_node(root, compute, context, result);
    return result;
}

} // namespace tessera::ui::style
