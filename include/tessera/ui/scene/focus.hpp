// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/node.hpp"

#include <unordered_map>

namespace tessera::ui {

// Tracks the keyboard focus target for a scene tree. Focus changes are dispatched through the
// normal capture/target/bubble path so ancestors can observe them. The manager resolves nodes by
// `NodeId` under the scene root on every operation, so a removed subtree cannot be dereferenced;
// it still stores raw pointers and does not observe destruction, so clear focus before destroying
// a subtree that currently holds it.
//
// `FocusVisible` (the `:focus-visible` heuristic bit) is owned here too. `request_focus` always
// clears it on the node it focuses, because a programmatic request carries no input modality: the
// caller must call `set_focus_visible(true)` when the focus came from keyboard interaction (the
// `InputRouter` does this after a Tab/Shift+Tab traversal and leaves it clear for pointer focus).
// `clear_focus`/`reset` clear both `Focus` and `FocusVisible`; moving focus clears both bits on the
// previous node. Direct `focus_next`/`focus_previous` calls count as programmatic.
//
// Sequential tab order (`focus_next` / `focus_previous`):
//   1. focusable descendants with a positive `tab_index`, ascending; equal values keep depth-first
//      tree order (pre-order);
//   2. focusable descendants with `tab_index == 0`, in depth-first tree order.
// Negative `tab_index` values are skipped by sequential traversal but remain valid targets for
// `request_focus` (programmatic focus). Invisible subtrees are skipped; non-focusable nodes are
// skipped while their children are still visited. Both ends wrap.
class FocusManager {
public:
    // Defines the scene root used to decide whether a node is attached. Requesting focus on the
    // root itself is only accepted after the root is known.
    void set_root(Node& root);

    // Focuses a visible, attached, focusable node inside the active modal subtree (if any).
    // Invalid requests are rejected without changing the current focus. `Focus` is set on the
    // target; `FocusVisible` is cleared on it (and on the previous target) because a programmatic
    // request has no modality.
    //
    // A successful request also scrolls the node into view: after the Focus event every scrollable
    // ancestor is adjusted (innermost to outermost) so the node's border box is inside that
    // ancestor's scrollport. An already-visible node changes no offset, and programmatic requests
    // scroll exactly like keyboard traversal (the reveal is not tied to the input modality).
    bool request_focus(Node* node);
    void clear_focus();

    // Sets or clears `PseudoState::FocusVisible` on the current focus target (no-op without focus).
    // The input layer uses this to mark keyboard-driven focus; programmatic callers use it to
    // restore the ring explicitly. The change marks `Dirty::Style` through `set_pseudo_state`.
    void set_focus_visible(bool visible);

    [[nodiscard]] Node* focused() const {
        return focused_;
    }

    bool focus_next(Node& root);
    bool focus_previous(Node& root);

    // Honours JSX `autoFocus` (issue #226): focuses the first visible, focusable node under `root`
    // in tree order whose `auto_focus()` is set, unless a node already holds focus. Hosts call it
    // once after the first layout of a tree. Returns true when focus moved.
    bool apply_auto_focus(Node& root);

    // Keeps the modal trap in step with the tree's `focus_trap` nodes (issue #236): the last
    // visible one in tree order becomes the modal root, and when none is left a trap this method
    // set is released, which restores the focus held before it. A trap set through `set_modal_root`
    // is never released here. Hosts call it every frame and the input router before each key event.
    void sync_focus_trap(Node& root);

    // Moves focus among the items of the nearest roving-focus group around the focused node when
    // `key` is an arrow of the group's axis (or Home/End), wrapping at both ends. Returns true
    // when focus moved.
    bool move_in_focus_group(Node& root, KeyCode key);

    // Restricts focus to `node` and its descendants. Setting a trap saves the current focus when
    // it is outside the new subtree and moves focus to the first sequential candidate inside it
    // (or clears focus when the subtree has no candidates). `nullptr` is equivalent to
    // `clear_modal_root`.
    //
    // `clear_modal_root` releases the trap and restores the saved focus when that node is still
    // attached, visible, focusable, and allowed by the current trap state; otherwise the current
    // focus stays where it is.
    //
    // A modal root that is removed or destroyed without an explicit clear is detected by the next
    // focus operation through the id lookup: the trap is released and the saved focus restored.
    // Returns false (leaving state unchanged) when `node` is not attached, visible, or has an
    // invisible ancestor.
    bool set_modal_root(Node* node);
    void clear_modal_root();

    // Drops focus and any modal trap without restoring the saved focus. Dispatches `Blur` for the
    // current target when it is still reachable, and emits no `Focus`.
    void reset();

    // Resolves the active modal root, or nullptr when no trap is active (or the saved root is no
    // longer part of the scene root).
    [[nodiscard]] Node* modal_root() const;

    // True when `node` may receive focus under the active trap (always true without a trap).
    [[nodiscard]] bool allows_focus(const Node& node) const;

private:
    [[nodiscard]] bool is_attached(const Node& node) const;
    [[nodiscard]] bool is_valid(const Node& node) const;
    [[nodiscard]] bool is_within_modal(const Node& node) const;
    [[nodiscard]] Node* find_node(NodeId id) const;
    void refresh_state();
    void release_modal_trap();
    bool focus_first_in_modal();
    bool move_focus(Node& root, bool forward);

    bool trap_owned_ = false;
    // Roving-focus group id -> the item that holds its Tab stop (issue #236).
    std::unordered_map<NodeId, NodeId> roving_active_;
    Node* focused_ = nullptr;
    NodeId focused_id_ = 0;
    Node* root_ = nullptr;
    Node* modal_root_ = nullptr;
    NodeId modal_root_id_ = 0;
    Node* saved_focus_ = nullptr;
    NodeId saved_focus_id_ = 0;
};

} // namespace tessera::ui
