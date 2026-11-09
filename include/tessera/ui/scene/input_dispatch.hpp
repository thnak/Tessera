// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

struct InputDispatchResult {
    bool dispatched = false;
    EventType type = EventType::PointerMove;
    Node* target = nullptr;
    // True when a handler set `prevent_default` and the dispatcher therefore skipped its default
    // action (currently only Tab focus traversal).
    bool default_prevented = false;
    // True when the default action actually moved focus (Tab/Shift+Tab).
    bool focus_moved = false;
};

InputDispatchResult dispatch_input(Node& root, const InputEvent& input);

// Routes normalized input to the scene graph and owns the scene's interaction state.
//
// Single-pointer interaction state:
// - `hovered()` is the deepest hit target of the last `PointerMove` and carries
//   `PseudoState::Hover`. The previous hover target loses the bit on the next move, including a
//   move that hits nothing (pointer left the scene). Hover is derived from hit testing even while
//   a pointer is captured, but it is only updated by moves, not by down/up.
// - `active()` is the `PointerDown` hit target and carries `PseudoState::Active` until
//   `PointerUp`, `PointerCancel`, or a window `Focus`/`Blur` input event.
// - A window `Focus`/`Blur` input event drops capture, hover, and active: the pointer can no
//   longer be considered over the scene. Scene focus is owned by `FocusManager` and is not
//   changed by those input events.
// - Ancestor hover is not maintained; see docs/design/input.md section 7 for the rule and why.
//
// Pseudo-state changes go through `Node::set_pseudo_state`, which marks `Dirty::Style` so
// `:hover`/`:active` rules re-resolve. `hovered()`, `active()`, and `captured()` are raw scene
// pointers: reset or destroy the router before destroying a subtree that holds any of them.
class InputRouter {
public:
    InputDispatchResult dispatch(Node& root, const InputEvent& input);
    void reset();

    [[nodiscard]] Node* captured() const {
        return captured_;
    }
    [[nodiscard]] Node* hovered() const {
        return hovered_;
    }
    [[nodiscard]] Node* active() const {
        return active_;
    }

    [[nodiscard]] FocusManager& focus_manager() {
        return focus_manager_;
    }
    [[nodiscard]] const FocusManager& focus_manager() const {
        return focus_manager_;
    }

private:
    void set_hover(Node* target);
    void set_active(Node* target);

    Node* captured_ = nullptr;
    Node* hovered_ = nullptr;
    Node* active_ = nullptr;
    FocusManager focus_manager_;
};

} // namespace tessera::ui
