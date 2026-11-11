// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui {

// Decides when a press/release pair, a key, or an invoke becomes a `Click` (issue #6). The
// InputRouter owns one and does the dispatching; this class holds only the bounded press state and
// the pure rules, so the rules are unit-testable without a scene.
//
// Pointer rule (DOM-like): a click needs a press and a release of the same primary pointer, the
// left button (or a touch/pen contact), no slop excursion, and a release that hits inside the
// scene. It fires on the nearest common ancestor of the pressed node and the node under the release
// point, so press-A-release-B never clicks A (it bubbles from their shared parent, where only a
// container handler hears it), while press-on-label-release-on-button clicks the button. A cancel,
// a window focus change or a lost press never clicks.
//
// Slop: touch and pen contacts that travel more than the slop radius from the press point are
// pans, not clicks. A mouse has no slop (release on the same element still clicks after a drag),
// matching the DOM.
//
// Keyboard rule: Enter on key-down (not auto-repeat) and Space on key-up after its own key-down
// activate the focused button (element kind `Button` or role `button`) unless the key event was
// consumed or Control/Alt/Meta is held.
class ClickSynthesizer {
public:
    // Bounded like the router's capture map: a press past the bound is dispatched normally but can
    // never become a click.
    static constexpr std::size_t kMaxPresses = 8;
    static constexpr float kTouchSlop = 10.0f;
    static constexpr float kPenSlop = 6.0f;

    struct Press {
        std::uint32_t pointer_id = 0;
        NodeId node_id = 0;
        Node* node = nullptr;
        float x = 0.0f;
        float y = 0.0f;
        PointerKind kind = PointerKind::Mouse;
        bool exceeded_slop = false;
    };

    // Records a primary-pointer press on `target`. Non-primary pointers and non-left mouse buttons
    // are ignored and can never click.
    void press(Node& target, const InputEvent& input, bool primary);
    // Marks the press as a pan once a touch/pen contact leaves the slop radius.
    void move(const InputEvent& input);
    // Removes and returns the press for `input.pointer_id`; false when none is recorded.
    bool take(std::uint32_t pointer_id, Press& out);
    void cancel(std::uint32_t pointer_id);
    void clear();

    [[nodiscard]] static bool button_can_click(PointerButton button) {
        return button == PointerButton::Left || button == PointerButton::None;
    }
    // The deepest node that is `a` or an ancestor of both; null when either is null or the nodes
    // live in different trees.
    [[nodiscard]] static Node* common_ancestor(Node* a, Node* b);
    // True for nodes Enter/Space activate: element kind `Button` or role `button`.
    [[nodiscard]] static bool keyboard_activatable(const Node& node);

    // Feeds one key event for the focused node. Returns true when it completes an activation:
    // Enter key-down, or Space key-up following Space key-down on the same node.
    bool key(InputEventType type, KeyCode key, bool repeat, Modifier modifiers, const Node* focused,
             bool consumed);

private:
    std::array<Press, kMaxPresses> presses_{};
    std::size_t press_count_ = 0;
    NodeId space_armed_id_ = 0;
};

} // namespace tessera::ui
