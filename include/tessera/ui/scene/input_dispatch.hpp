// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/input/key_bindings.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui {

struct InputDispatchResult {
    bool dispatched = false;
    EventType type = EventType::PointerMove;
    Node* target = nullptr;
    // Stable NodeId of `target` at dispatch time (0 when nothing was dispatched). Additive: the
    // gesture pipeline consumes this so gesture targets are ids, never raw pointers.
    NodeId target_id = 0;
    // True when a handler set `prevent_default` and the dispatcher therefore skipped its default
    // action (currently only Tab focus traversal).
    bool default_prevented = false;
    // True when the default action actually moved focus (Tab/Shift+Tab).
    bool focus_moved = false;
    // BindingId of the accelerator that fired for this key event (0 when none). An accelerator
    // fires only for KeyDown after the focused node's handler and only when the event was not
    // consumed; a fired accelerator suppresses the router's Tab default action.
    BindingId accelerator_fired = kInvalidBindingId;
};

InputDispatchResult dispatch_input(Node& root, const InputEvent& input);

// Routes normalized input to the scene graph and owns the scene's interaction state.
//
// Pointer capture (bounded, allocation-free):
// - Pointers are identified by `InputEvent::pointer_id`. `PointerDown` implicitly captures the hit
//   target for that pointer; `set_pointer_capture`/`release_pointer_capture` expose the same state
//   to custom elements and tests. The map holds at most `kMaxPointerCaptures` pointers; a press
//   past that bound is still dispatched but is not captured.
// - Captures store `{pointer_id, NodeId}` and are resolved under the last dispatched scene root on
//   every use, so a captured subtree removed from (or destroyed with) the scene drops its capture
//   instead of leaving a dangling pointer behind.
// - `PointerMove`/`PointerUp`/`PointerCancel` route to that pointer's capture target, else to the
//   hit target. Releasing one pointer never affects another. `PointerCancel` releases only that
//   pointer; a window `Focus`/`Blur` input event clears every capture.
//
// Hover/active (primary pointer only):
// - The primary pointer is the pointer that went down while no other pointer was captured; it is
//   pointer 0 before the first press. When it goes up or is cancelled, the oldest remaining
//   capture (insertion order) becomes primary, or pointer 0 when none remain. Only the primary
//   pointer drives `Hover`/`Active`.
// - `hovered()` is the deepest hit target of the primary pointer's last `PointerMove` and carries
//   `PseudoState::Hover`. The previous hover target loses the bit on the next move, including a
//   move that hits nothing (pointer left the scene). A hover change also fires the W3C boundary
//   events: `PointerOut` on the old target (bubbling to the common ancestor exclusive),
//   `PointerLeave` per old-chain node below it, then `PointerOver`/`PointerEnter` mirrored for the
//   new chain. `PointerEnter`/`PointerLeave` do not bubble and fire outermost->innermost /
//   innermost->outermost respectively. Touch never fires them.
// - A captured pointer freezes the hover chain for that pointer: its moves still dispatch to the
//   capture target but do not change `hovered()`, the `Hover` bits, or fire boundary events until
//   the capture is released and the pointer moves again.
// - `active()` is the primary pointer's `PointerDown` hit target and carries `PseudoState::Active`
//   until that pointer goes up or is cancelled (or `Focus`/`Blur`/`reset`).
// - A window `Focus`/`Blur` input event drops all captures, the hover chain (firing out/leave), and
//   active: the pointer can no longer be considered over the scene. A primary mouse/pen
//   `PointerCancel` also leaves the hover chain the same way; a touch cancel never owns hover.
//   Scene focus is owned by `FocusManager` and is not changed by those input events.
// - Hover/active targets are also validated by `NodeId` before their bit is cleared, so a removed
//   or destroyed target is never dereferenced. Only the deepest target is hovered; see
//   docs/design/input.md section 7 for the ancestor-chain rule and why it is deferred.
//
// Pointer kinds:
// - `InputEvent::pointer_kind` is forwarded to the scene event. Mouse and pen drive hover (when
//   they are the primary pointer); touch never does, because a contact has no pre-contact hover
//   position. A touch/pen `PointerDown` still becomes `active` and is captured like a mouse
//   press, and touch/pen focus remains pointer modality, so `:focus-visible` stays clear.
// - Capture stays keyed by `pointer_id` only. Ids are document-scoped across kinds: a producer
//   must not reuse an id for two live pointers, and the sampler never coalesces moves of
//   different kinds even if an id is reused.
//
// Focus modality (`:focus-visible`):
// - Pointer-driven focus clears `PseudoState::FocusVisible` on the newly focused node; the
//   Tab/Shift+Tab default action sets it. The `FocusManager` clears the bit on every
//   `request_focus`, so a programmatic request is treated as having no modality; the router calls
//   `FocusManager::set_focus_visible(true)` after a keyboard traversal (also when a Blur handler
//   redirected focus). See docs/design/input.md section 6 for the exact rule.
//
// Keyboard and text:
// - `KeyDown`/`KeyUp` go to the focused node through the normal propagation path; with no focus
//   they are not dispatched.
// - Application accelerators (`KeyBindingRegistry`) are evaluated for `KeyDown` after the focused
//   node's handler and only when it did not consume the event (`handled` or `prevent_default`).
//   Chord matching is exact by default (broad/superset and repeat opt-in per binding), and the
//   first matching binding fires. A fired accelerator is reported in
//   `InputDispatchResult::accelerator_fired` and suppresses the Tab focus default action. With no
//   bindings registered key behaviour is unchanged.
// - Single-codepoint `Text` input and the IME composition family (`CompositionStart`/`Update`/
//   `End`, plus multi-codepoint `EditText`) go to the focused node as the matching scene
//   `EventType`, carrying the bounded inline UTF-8 payload and the UTF-16 code-unit selection
//   range. Without focus they are dropped. `prevent_default` is advisory for these events: there
//   is no framework text insertion yet, so it has no default action to suppress.
//
// Pseudo-state changes go through `Node::set_pseudo_state`, which marks `Dirty::Style` so
// `:hover`/`:active` rules re-resolve. `hovered()` and `active()` are raw scene pointers: reset or
// destroy the router before destroying a subtree that currently carries one of those states.
class InputRouter {
public:
    // Upper bound on simultaneously captured pointers. Implicit capture on a press beyond the
    // bound is skipped; explicit `set_pointer_capture` reports it in its return value.
    static constexpr std::size_t kMaxPointerCaptures = 8;

    InputDispatchResult dispatch(Node& root, const InputEvent& input);
    void reset();

    // Captures `node` for `pointer_id`, replacing an existing entry. Returns false when `node` is
    // null or when a new entry is needed and the bounded map is full. Explicit capture neither
    // changes the primary pointer nor dispatches anything; the capture ends on that pointer's
    // `PointerUp`/`PointerCancel`, `release_pointer_capture`, or a window `Focus`/`Blur` reset.
    bool set_pointer_capture(Node* node, std::uint32_t pointer_id);
    // Drops the entry for `pointer_id` without touching any other pointer. Does not change the
    // primary pointer; primary hand-off happens only on `PointerUp`/`PointerCancel`.
    void release_pointer_capture(std::uint32_t pointer_id);

    // Capture target for `pointer_id`, resolved under the last dispatched scene root. Null when
    // the pointer is not captured, its entry was released, or the captured node was removed from
    // the scene.
    [[nodiscard]] Node* captured(std::uint32_t pointer_id) const;
    // Single-pointer view used before per-pointer capture: the primary pointer's capture target.
    [[nodiscard]] Node* captured() const {
        return captured(primary_pointer_id_);
    }
    [[nodiscard]] std::uint32_t primary_pointer_id() const {
        return primary_pointer_id_;
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

    // Application-level keyboard accelerators. The registry is owned by the router and survives
    // `reset()` (bindings are app state, not scene interaction state).
    [[nodiscard]] KeyBindingRegistry& accelerators() {
        return accelerators_;
    }
    [[nodiscard]] const KeyBindingRegistry& accelerators() const {
        return accelerators_;
    }

private:
    struct CaptureEntry {
        std::uint32_t pointer_id = 0;
        NodeId node_id = 0;
        Node* node = nullptr;
    };

    void set_hover(Node* target, const InputEvent& input);
    void clear_hover(bool emit_events, const InputEvent& input);
    void set_active(Node* target);
    void clear_active();
    // Resolves a stored entry under `root_`; returns null when the node left the scene. Never
    // dereferences `entry.node` unless it is found under the root.
    [[nodiscard]] Node* resolve(const CaptureEntry& entry) const;
    // Resolves (and prunes a stale entry for) one pointer's capture target.
    Node* take_capture_target(std::uint32_t pointer_id);
    void remove_capture(std::uint32_t pointer_id);
    void clear_captures();
    void promote_primary_after(std::uint32_t pointer_id);

    Node* root_ = nullptr;
    Node* hovered_ = nullptr;
    NodeId hovered_id_ = 0;
    Node* active_ = nullptr;
    NodeId active_id_ = 0;
    std::uint32_t primary_pointer_id_ = 0;
    std::array<CaptureEntry, kMaxPointerCaptures> captures_{};
    std::size_t capture_count_ = 0;
    FocusManager focus_manager_;
    KeyBindingRegistry accelerators_;
};

} // namespace tessera::ui
