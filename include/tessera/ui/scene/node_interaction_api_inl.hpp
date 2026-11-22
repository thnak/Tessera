// SPDX-License-Identifier: MIT

#pragma once

// Inline half of `NodeInteractionApi`. Included only by `node.hpp`, after `Node` is complete: each
// accessor is a one-line read of `Node`'s storage and stays inline so the input and focus hot paths
// pay no call for it. Do not include this file directly.

namespace tessera::ui {

inline Node& NodeInteractionApi::node() noexcept {
    return static_cast<Node&>(*this);
}
inline const Node& NodeInteractionApi::node() const noexcept {
    return static_cast<const Node&>(*this);
}

inline bool NodeInteractionApi::visible() const {
    return node().flag(Node::kVisible);
}
inline bool NodeInteractionApi::style_hidden() const {
    return node().flag(Node::kStyleHidden);
}
inline bool NodeInteractionApi::rendered() const {
    return node().flag(Node::kVisible) && !node().flag(Node::kStyleHidden);
}
inline bool NodeInteractionApi::pointer_events() const {
    return node().flag(Node::kPointerEvents);
}
inline UserSelect NodeInteractionApi::user_select() const {
    return node().user_select_;
}
inline style::CursorKind NodeInteractionApi::cursor() const {
    return node().cursor_;
}
inline bool NodeInteractionApi::focusable() const {
    return node().flag(Node::kFocusable);
}
inline int NodeInteractionApi::tab_index() const {
    return node().tab_index_;
}
inline void NodeInteractionApi::set_auto_focus(bool auto_focus) {
    node().set_flag(Node::kAutoFocus, auto_focus);
}
inline bool NodeInteractionApi::auto_focus() const {
    return node().flag(Node::kAutoFocus);
}
inline void NodeInteractionApi::set_focus_trap(bool trap) {
    node().set_flag(Node::kFocusTrap, trap);
}
inline bool NodeInteractionApi::focus_trap() const {
    return node().flag(Node::kFocusTrap);
}
inline void NodeInteractionApi::set_typeahead(bool typeahead) {
    node().set_flag(Node::kTypeahead, typeahead);
}
inline bool NodeInteractionApi::typeahead() const {
    return node().flag(Node::kTypeahead);
}
inline void NodeInteractionApi::set_interactive_parent(bool interactive) {
    node().set_flag(Node::kInteractiveParent, interactive);
}
inline bool NodeInteractionApi::interactive_parent() const {
    return node().flag(Node::kInteractiveParent);
}
inline void NodeInteractionApi::set_focus_group(FocusGroup group) {
    node().focus_group_ = group;
}
inline FocusGroup NodeInteractionApi::focus_group() const {
    return node().focus_group_;
}
inline style::PseudoState NodeInteractionApi::pseudo_state() const {
    return node().pseudo_state_;
}

} // namespace tessera::ui
