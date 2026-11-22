// SPDX-License-Identifier: MIT

// Interaction-policy half of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1, slice
// 1): visibility, hit eligibility, focus reachability, selection policy, the cursor the app reports
// for a hovered node, and the pseudo-class bits the style pass matches on. These are the inputs the
// input router and focus manager read; they are not geometry and not scroll.

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

void Node::set_visible(bool visible) {
    if (flag(kVisible) == visible) {
        return;
    }
    set_flag(kVisible, visible);
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
}

void Node::set_style_hidden(bool hidden) {
    if (flag(kStyleHidden) == hidden) {
        return;
    }
    set_flag(kStyleHidden, hidden);
    // Paint and render only: unlike `display: none`, `visibility: hidden` keeps the node's layout
    // box, so the parent must not reflow. Hit testing is not a dirty domain (the next query reads
    // the flag directly), and the accessibility tree is rebuilt on demand.
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_focusable(bool focusable) {
    if (flag(kFocusable) == focusable) {
        return;
    }
    set_flag(kFocusable, focusable);
    mark_dirty(Dirty::Style);
}

void Node::set_pointer_events(bool enabled) {
    // Hit testing is not a dirty domain: the next input query reads the flag directly, so nothing
    // is repainted and no damage is raised by a stylesheet-only change.
    set_flag(kPointerEvents, enabled);
}

void Node::set_user_select(UserSelect value) {
    user_select_ = value;
}

void Node::set_cursor(style::CursorKind value) {
    // No dirty domain: a cursor change paints nothing and moves no box. The app reads the hovered
    // node's value after input, so marking the frame dirty here would only cause phantom paints. A
    // no-op when unchanged keeps a steady scene clean.
    if (cursor_ == value) {
        return;
    }
    cursor_ = value;
}

bool Node::text_selection_allowed() const {
    // A text field stays selectable even inside a `user-select: none` subtree, as in CSS: `none`
    // suppresses the document's selection gesture, it does not disable an editable control.
    if (element_kind() == "TextInput" || element_kind() == "TextArea") {
        return true;
    }
    return user_select_ != UserSelect::None;
}

void Node::set_tab_index(int tab_index) {
    tab_index_ = tab_index;
}

void Node::set_pseudo_state(style::PseudoState state) {
    if (pseudo_state_ == state) {
        return;
    }
    pseudo_state_ = state;
    mark_dirty(Dirty::Style);
}

void Node::set_pseudo_state(style::PseudoState state, bool enabled) {
    const style::PseudoState next = enabled ? (pseudo_state_ | state) : (pseudo_state_ & ~state);
    set_pseudo_state(next);
}

} // namespace tessera::ui
