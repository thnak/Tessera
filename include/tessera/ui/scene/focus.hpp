// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// Tracks the keyboard focus target for a scene tree. Focus changes are dispatched through the
// normal capture/target/bubble path so ancestors can observe them. The manager stores raw node
// pointers and does not observe destruction: destroy it before the scene root, and clear focus
// before destroying the focused subtree.
class FocusManager {
public:
    // Defines the scene root used to decide whether a node is attached. Requesting focus on the
    // root itself is only accepted after the root is known.
    void set_root(Node& root);

    // Focuses a visible, attached, focusable node. Invalid requests are rejected without changing
    // the current focus.
    bool request_focus(Node* node);
    void clear_focus();

    [[nodiscard]] Node* focused() const {
        return focused_;
    }

    // Moves focus in depth-first document order (pre-order), wrapping at both ends. Invisible
    // subtrees are skipped; non-focusable nodes are skipped but their children are visited.
    bool focus_next(Node& root);
    bool focus_previous(Node& root);

private:
    [[nodiscard]] bool is_attached(const Node& node) const;
    [[nodiscard]] bool is_valid(const Node& node) const;
    bool move_focus(Node& root, bool forward);

    Node* focused_ = nullptr;
    Node* root_ = nullptr;
};

} // namespace tessera::ui
