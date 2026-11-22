// SPDX-License-Identifier: MIT

#pragma once

// Inline half of `NodeInteractionApi`. Included only by `node.hpp`, after `Node` is complete: each
// accessor is a one-line read of `NodeInputPolicy` and stays inline so the input and focus hot
// paths pay no call for it. Do not include this file directly.

namespace tessera::ui {

inline Node& NodeInteractionApi::node() noexcept {
    return static_cast<Node&>(*this);
}
inline const Node& NodeInteractionApi::node() const noexcept {
    return static_cast<const Node&>(*this);
}

inline bool NodeInteractionApi::visible() const {
    return node().input_.visible();
}
inline bool NodeInteractionApi::style_hidden() const {
    return node().input_.style_hidden();
}
inline bool NodeInteractionApi::rendered() const {
    return node().input_.rendered();
}
inline bool NodeInteractionApi::pointer_events() const {
    return node().input_.pointer_events();
}
inline UserSelect NodeInteractionApi::user_select() const {
    return node().input_.user_select();
}
inline style::CursorKind NodeInteractionApi::cursor() const {
    return node().input_.cursor();
}
inline bool NodeInteractionApi::focusable() const {
    return node().input_.focusable();
}
inline int NodeInteractionApi::tab_index() const {
    return node().tab_index_;
}
inline void NodeInteractionApi::set_auto_focus(bool auto_focus) {
    node().input_.set_auto_focus(auto_focus);
}
inline bool NodeInteractionApi::auto_focus() const {
    return node().input_.auto_focus();
}
inline void NodeInteractionApi::set_focus_trap(bool trap) {
    node().input_.set_focus_trap(trap);
}
inline bool NodeInteractionApi::focus_trap() const {
    return node().input_.focus_trap();
}
inline void NodeInteractionApi::set_typeahead(bool typeahead) {
    node().input_.set_typeahead(typeahead);
}
inline bool NodeInteractionApi::typeahead() const {
    return node().input_.typeahead();
}
inline void NodeInteractionApi::set_interactive_parent(bool interactive) {
    node().input_.set_interactive_parent(interactive);
}
inline bool NodeInteractionApi::interactive_parent() const {
    return node().input_.interactive_parent();
}
inline void NodeInteractionApi::set_focus_group(FocusGroup group) {
    node().input_.set_focus_group(group);
}
inline FocusGroup NodeInteractionApi::focus_group() const {
    return node().input_.focus_group();
}
inline style::PseudoState NodeInteractionApi::pseudo_state() const {
    return node().pseudo_state_;
}

} // namespace tessera::ui
