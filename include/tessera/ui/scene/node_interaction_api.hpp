// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node_types.hpp"
#include "tessera/ui/style/cursor_value.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

// Interaction-policy surface of `ui::Node` (docs/architecture/god-type-audit-v1.md section 3.1):
// visibility, hit eligibility, focus reachability, selection policy, the cursor the app reports and
// the pseudo-class bits selector matching reads. These are the inputs the input router, the focus
// manager and the style pass read.
//
// This is a base of `Node`, not a separate object: the storage (one flag word, three small scalars
// and the pseudo-state) stays inline in `Node` so `kNodeSizeBudget` and the exact-size assert keep
// their meaning, and the class has no data and no virtual member so it adds no byte. The bodies
// live in `node_interaction.cpp` and `node_interaction_api_inl.hpp`; both reach the storage through
// `node()`, which is why `Node` befriends this type.
namespace tessera::ui {

class Node;

class NodeInteractionApi {
public:
    // Programmatic/structural gate (a conditional branch, a recycled virtual item).
    void set_visible(bool visible);
    [[nodiscard]] bool visible() const;

    // CSS `visibility: hidden|collapse` (stylesheet-v1 section 11, issue #213), copied from
    // computed style by `style::sync_interaction_style`. It is a separate flag from `visible()`
    // because the two have different owners: `visible()` is the programmatic/structural gate, while
    // this one is a stylesheet value that must be able to flip back without resurrecting a node the
    // program hid. V1 treats it like `display: none` for painting, hit testing and the
    // accessibility tree (the subtree is skipped, so a descendant cannot re-enable with
    // `visibility: visible`), but the node keeps its layout box.
    void set_style_hidden(bool hidden);
    [[nodiscard]] bool style_hidden() const;
    // True when this node takes part in painting, hit testing and the accessibility tree: both the
    // programmatic gate and the stylesheet value must allow it.
    [[nodiscard]] bool rendered() const;

    // CSS `pointer-events: auto|none` (stylesheet-v1 section 11, issue #247), copied from computed
    // style by `style::sync_interaction_style`. False makes `hit_test_deep` pass this node over as
    // a target: hover, active, capture, click synthesis, wheel and accessibility-free hit tests
    // land on whatever is behind it. The subtree is still searched, so a descendant with `auto` is
    // hit again. Painting, keyboard focus and the accessibility tree are unaffected.
    void set_pointer_events(bool enabled);
    [[nodiscard]] bool pointer_events() const;

    void set_user_select(UserSelect value);
    [[nodiscard]] UserSelect user_select() const;
    // True when a selection gesture may start at this node: false only for an explicit/inherited
    // `user-select: none`. Editable text (`TextInput`/`TextArea`) always allows selection, whatever
    // an ancestor's `none` says, as in CSS.
    [[nodiscard]] bool text_selection_allowed() const;

    // CSS `cursor` (stylesheet-v1 section 11, issue #191), copied from computed style by
    // `style::sync_interaction_style`. The scene stores the resolved enum, never the CSS text, so
    // hit testing, input and the platform seam read it without parsing. The property is inherited,
    // so the computed value already carries the ancestor resolution: a `pointer` parent with an
    // explicit `auto` child yields `Auto` on the child. The setter is a no-op when unchanged, so a
    // steady scene stays clean; it marks no dirty domain (no pixels change).
    void set_cursor(style::CursorKind value);
    [[nodiscard]] style::CursorKind cursor() const;

    void set_focusable(bool focusable);
    [[nodiscard]] bool focusable() const;

    // Sequential focus order key. The ordering contract lives on FocusManager::focus_next.
    void set_tab_index(int tab_index);
    [[nodiscard]] int tab_index() const;

    void set_auto_focus(bool auto_focus);
    [[nodiscard]] bool auto_focus() const;

    // JSX `focusTrap` and `focusGroup` (issue #236): a `focus_trap` node confines Tab traversal to
    // its subtree and restores the previous focus when it goes away; a `focus_group` node is a
    // roving-focus group whose focusable descendants form one Tab stop.
    void set_focus_trap(bool trap);
    [[nodiscard]] bool focus_trap() const;
    void set_typeahead(bool typeahead);
    [[nodiscard]] bool typeahead() const;
    void set_interactive_parent(bool interactive);
    [[nodiscard]] bool interactive_parent() const;
    void set_focus_group(FocusGroup group);
    [[nodiscard]] FocusGroup focus_group() const;

    // Interaction state seen by selector matching. Focus changes update the `Focus` bit and mark
    // the style domain dirty so the resolver re-evaluates `:focus`/`:not(:focus)`.
    void set_pseudo_state(style::PseudoState state);
    void set_pseudo_state(style::PseudoState state, bool enabled);
    [[nodiscard]] style::PseudoState pseudo_state() const;

protected:
    // Not constructible or destroyed through the interface: only `Node` derives from it.
    NodeInteractionApi() = default;
    ~NodeInteractionApi() = default;

private:
    // Defined in `node_interaction_api_inl.hpp`, once `Node` is complete.
    [[nodiscard]] Node& node() noexcept;
    [[nodiscard]] const Node& node() const noexcept;
};

} // namespace tessera::ui
