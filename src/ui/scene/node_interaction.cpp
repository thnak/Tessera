// SPDX-License-Identifier: MIT

// Interaction-policy facade of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1):
// visibility, hit eligibility, focus reachability, selection policy, the cursor the app reports
// for a hovered node, and the pseudo-class bits the style pass matches on. The values live in
// `NodeInputPolicy`; the bodies here own the frame policy, i.e. which dirty domain each change
// raises.

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

void NodeInteractionApi::set_visible(bool visible) {
    Node& self = node();
    if (self.input_.set_visible(visible)) {
        self.mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    }
}

void NodeInteractionApi::set_style_hidden(bool hidden) {
    Node& self = node();
    if (self.input_.set_style_hidden(hidden)) {
        // Paint and render only: unlike `display: none`, `visibility: hidden` keeps the node's
        // layout box, so the parent must not reflow. Hit testing is not a dirty domain (the next
        // query reads the flag directly), and the accessibility tree is rebuilt on demand.
        self.mark_dirty(Dirty::Paint | Dirty::Render);
    }
}

void NodeInteractionApi::set_focusable(bool focusable) {
    Node& self = node();
    if (self.input_.set_focusable(focusable)) {
        self.mark_dirty(Dirty::Style);
    }
}

void NodeInteractionApi::set_pointer_events(bool enabled) {
    // Hit testing is not a dirty domain: the next input query reads the flag directly, so nothing
    // is repainted and no damage is raised by a stylesheet-only change.
    node().input_.set_pointer_events(enabled);
}

void NodeInteractionApi::set_user_select(UserSelect value) {
    node().input_.set_user_select(value);
}

void NodeInteractionApi::set_cursor(style::CursorKind value) {
    // No dirty domain: a cursor change paints nothing and moves no box. The app reads the hovered
    // node's value after input, so marking the frame dirty here would only cause phantom paints. A
    // no-op when unchanged keeps a steady scene clean.
    node().input_.set_cursor(value);
}

bool NodeInteractionApi::text_selection_allowed() const {
    const Node& self = node();
    // A text field stays selectable even inside a `user-select: none` subtree, as in CSS: `none`
    // suppresses the document's selection gesture, it does not disable an editable control.
    if (self.element_kind() == "TextInput" || self.element_kind() == "TextArea") {
        return true;
    }
    return self.input_.user_select() != UserSelect::None;
}

void NodeInteractionApi::set_tab_index(int tab_index) {
    node().tab_index_ = tab_index;
}

void NodeInteractionApi::set_pseudo_state(style::PseudoState state) {
    Node& self = node();
    if (self.pseudo_state_ == state) {
        return;
    }
    self.pseudo_state_ = state;
    self.mark_dirty(Dirty::Style);
}

void NodeInteractionApi::set_pseudo_state(style::PseudoState state, bool enabled) {
    const style::PseudoState current = node().pseudo_state_;
    const style::PseudoState next = enabled ? (current | state) : (current & ~state);
    set_pseudo_state(next);
}

} // namespace tessera::ui
