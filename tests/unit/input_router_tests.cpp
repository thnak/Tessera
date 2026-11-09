// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Dirty;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_router: " << message << "\n";
    }
    return condition;
}

const char* type_name(EventType type) {
    switch (type) {
    case EventType::PointerDown:
        return "down";
    case EventType::PointerUp:
        return "up";
    case EventType::PointerMove:
        return "move";
    case EventType::Wheel:
        return "wheel";
    case EventType::Text:
        return "text";
    case EventType::KeyDown:
        return "keydown";
    case EventType::KeyUp:
        return "keyup";
    case EventType::Focus:
        return "focus";
    case EventType::Blur:
        return "blur";
    case EventType::PointerOver:
        return "over";
    case EventType::PointerOut:
        return "out";
    case EventType::PointerEnter:
        return "enter";
    case EventType::PointerLeave:
        return "leave";
    case EventType::CompositionStart:
        return "composition-start";
    case EventType::CompositionUpdate:
        return "composition-update";
    case EventType::CompositionEnd:
        return "composition-end";
    case EventType::EditText:
        return "edit-text";
    case EventType::TextChange:
        return "text-change";
    }
    return "unknown";
}

const char* phase_name(EventPhase phase) {
    switch (phase) {
    case EventPhase::Capture:
        return "capture";
    case EventPhase::Target:
        return "target";
    case EventPhase::Bubble:
        return "bubble";
    }
    return "unknown";
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        log.push_back(std::to_string(id()) + ":" + type_name(event.type) + ":" +
                      phase_name(event.phase));
    }

    std::vector<std::string> log;
};

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

InputEvent lifecycle(InputEventType type) {
    InputEvent event;
    event.type = type;
    return event;
}

InputEvent pointer_for(InputEventType type, float x, float y, std::uint32_t pointer_id) {
    InputEvent event = pointer(type, x, y);
    event.pointer_id = pointer_id;
    return event;
}

bool style_dirty(const Node& node) {
    return tessera::ui::has_flag(node.dirty(), Dirty::Style);
}

void clear_dirty(Node& root, Node& child) {
    root.clear_dirty(Dirty::All);
    child.clear_dirty(Dirty::All);
}

// Hover/active state machine: deepest-target hover, press activation, and the cleanup paths.
bool interaction_state(Recorder& root, Recorder& child, InputRouter& router) {
    clear_dirty(root, child);

    // Hover enter: only the deepest hit target is hovered.
    const auto enter = router.dispatch(root, pointer(InputEventType::PointerMove, 20, 20));
    if (!expect(enter.dispatched && enter.target == &child, "hover enter hits child") ||
        !expect(router.hovered() == &child, "hover enter tracks child") ||
        !expect(has_state(child.pseudo_state(), PseudoState::Hover), "child hover bit set") ||
        !expect(!has_state(root.pseudo_state(), PseudoState::Hover), "root not hovered") ||
        !expect(style_dirty(child), "hover enter marks style dirty")) {
        return false;
    }

    // Repeating the same move neither changes pseudo-state nor churns dirty flags.
    clear_dirty(root, child);
    router.dispatch(root, pointer(InputEventType::PointerMove, 25, 25));
    if (!expect(router.hovered() == &child, "hover stays on child") ||
        !expect(!style_dirty(child) && !style_dirty(root), "same hover marks nothing dirty")) {
        return false;
    }

    // Move to another target: the previous hover target is cleared.
    const auto leave = router.dispatch(root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(leave.dispatched && leave.target == &root, "move hits root") ||
        !expect(router.hovered() == &root, "hover moves to root") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Hover), "previous hover cleared") ||
        !expect(has_state(root.pseudo_state(), PseudoState::Hover), "root hover bit set") ||
        !expect(style_dirty(child), "clearing hover marks style dirty")) {
        return false;
    }

    // Move outside the scene: no hit target, so hover is cleared.
    clear_dirty(root, child);
    const auto outside = router.dispatch(root, pointer(InputEventType::PointerMove, 500, 500));
    if (!expect(!outside.dispatched && outside.target == nullptr, "outside move not dispatched") ||
        !expect(router.hovered() == nullptr, "outside clears hover target") ||
        !expect(!has_state(root.pseudo_state(), PseudoState::Hover), "outside clears hover bit") ||
        !expect(style_dirty(root), "leaving the scene marks style dirty")) {
        return false;
    }

    // A hidden target is skipped by hit testing, so its ancestor is hovered instead.
    child.set_visible(false);
    clear_dirty(root, child);
    const auto hidden = router.dispatch(root, pointer(InputEventType::PointerMove, 20, 20));
    if (!expect(hidden.target == &root, "hidden child skipped") ||
        !expect(router.hovered() == &root, "hidden child not hovered") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Hover),
                "hidden child has no hover")) {
        return false;
    }
    child.set_visible(true);

    // Press: the hit target becomes active. Active ends on up even though hover stays.
    router.dispatch(root, pointer(InputEventType::PointerMove, 20, 20));
    clear_dirty(root, child);
    const auto down = router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(down.target == &child, "press hit target") ||
        !expect(router.active() == &child, "press becomes active") ||
        !expect(has_state(child.pseudo_state(), PseudoState::Active), "active bit set") ||
        !expect(style_dirty(child), "active marks style dirty")) {
        return false;
    }
    clear_dirty(root, child);
    const auto up = router.dispatch(root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(up.target == &child, "up hit target") ||
        !expect(router.active() == nullptr, "up clears active target") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Active), "up clears active bit") ||
        !expect(has_state(child.pseudo_state(), PseudoState::Hover), "up keeps hover bit") ||
        !expect(style_dirty(child), "clearing active marks style dirty")) {
        return false;
    }

    // Cancel ends the press and leaves the hover chain (out/leave fire for a mouse/pen pointer).
    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    const auto cancelled = router.dispatch(root, pointer(InputEventType::PointerCancel, 20, 20));
    if (!expect(!cancelled.dispatched, "cancel not dispatched") ||
        !expect(router.active() == nullptr, "cancel clears active target") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Active),
                "cancel clears active bit") ||
        !expect(router.hovered() == nullptr, "cancel clears hover target") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Hover), "cancel clears hover bit")) {
        return false;
    }

    // Window blur resets capture, hover, and active, but scene focus stays with FocusManager.
    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(router.captured() == &child, "blur precondition capture")) {
        return false;
    }
    const auto blurred = router.dispatch(root, lifecycle(InputEventType::Blur));
    if (!expect(!blurred.dispatched, "blur not dispatched") ||
        !expect(router.captured() == nullptr, "blur clears capture") ||
        !expect(router.hovered() == nullptr, "blur clears hover target") ||
        !expect(router.active() == nullptr, "blur clears active target") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Hover), "blur clears hover bit") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Active), "blur clears active bit") ||
        !expect(router.focus_manager().focused() == &child, "blur keeps scene focus")) {
        return false;
    }

    // A Focus input event behaves like Blur for interaction state.
    router.dispatch(root, pointer(InputEventType::PointerMove, 20, 20));
    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(root, lifecycle(InputEventType::Focus));
    if (!expect(router.captured() == nullptr && router.hovered() == nullptr &&
                    router.active() == nullptr,
                "focus input resets interaction state") ||
        !expect(!has_state(child.pseudo_state(), PseudoState::Active), "focus clears active bit")) {
        return false;
    }

    // Router reset drops interaction state as well.
    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    router.reset();
    return expect(router.active() == nullptr && router.hovered() == nullptr,
                  "reset clears interaction state") &&
           expect(!has_state(child.pseudo_state(), PseudoState::Active), "reset clears active bit");
}

// Per-pointer capture: two independent pointers, interleaving, hand-off, removal and overflow.
bool per_pointer_capture() {
    Recorder root(10);
    root.set_bounds({0, 0, 200, 200});
    auto left_ptr = std::make_unique<Recorder>(11);
    left_ptr->set_bounds({10, 10, 50, 50});
    auto right_ptr = std::make_unique<Recorder>(12);
    right_ptr->set_bounds({100, 100, 50, 50});
    Recorder& left = static_cast<Recorder&>(root.add_child(std::move(left_ptr)));
    Recorder& right = static_cast<Recorder&>(root.add_child(std::move(right_ptr)));

    InputRouter router;

    // First press becomes the primary pointer and implicitly captures its target.
    const auto down_left =
        router.dispatch(root, pointer_for(InputEventType::PointerDown, 20, 20, 1));
    if (!expect(down_left.target == &left, "pointer 1 presses left") ||
        !expect(router.captured(1) == &left, "pointer 1 implicitly captures left") ||
        !expect(router.captured() == &left, "primary capture view is pointer 1") ||
        !expect(router.primary_pointer_id() == 1, "first press becomes primary") ||
        !expect(router.active() == &left, "primary press activates left")) {
        return false;
    }

    // A second pointer gets an independent capture and does not steal primary or active.
    const auto down_right =
        router.dispatch(root, pointer_for(InputEventType::PointerDown, 120, 120, 2));
    if (!expect(down_right.target == &right, "pointer 2 presses right") ||
        !expect(router.captured(2) == &right, "pointer 2 captures right") ||
        !expect(router.captured(1) == &left, "pointer 2 keeps pointer 1 capture") ||
        !expect(router.primary_pointer_id() == 1, "second pointer is not primary") ||
        !expect(router.active() == &left, "second pointer does not steal active")) {
        return false;
    }

    // Interleaved moves route to each capture target; capture freezes the hover chain.
    left.log.clear();
    right.log.clear();
    const auto move_one =
        router.dispatch(root, pointer_for(InputEventType::PointerMove, 150, 150, 1));
    const auto move_two =
        router.dispatch(root, pointer_for(InputEventType::PointerMove, 20, 20, 2));
    if (!expect(move_one.target == &left, "pointer 1 move stays captured") ||
        !expect(move_two.target == &right, "pointer 2 move stays captured") ||
        !expect(router.hovered() == nullptr, "capture suppresses hover changes") ||
        !expect(left.log.size() == 1 && right.log.size() == 1, "both captures receive moves")) {
        return false;
    }

    // Releasing one pointer leaves the other capture and hands primary to the survivor.
    const auto up_one = router.dispatch(root, pointer_for(InputEventType::PointerUp, 150, 150, 1));
    if (!expect(up_one.target == &left, "pointer 1 up uses its capture") ||
        !expect(router.captured(1) == nullptr, "pointer 1 up releases its capture") ||
        !expect(router.captured(2) == &right, "pointer 2 capture survives") ||
        !expect(router.active() == nullptr, "primary up clears active") ||
        !expect(router.primary_pointer_id() == 2, "surviving pointer becomes primary")) {
        return false;
    }

    // A capture target removed from the scene is pruned, never dereferenced.
    auto removed = root.remove_child(right);
    if (!expect(removed != nullptr, "right removed from scene")) {
        return false;
    }
    const auto orphan_move =
        router.dispatch(root, pointer_for(InputEventType::PointerMove, 150, 150, 2));
    if (!expect(orphan_move.target == &root, "orphan capture falls back to hit target") ||
        !expect(router.captured(2) == nullptr, "orphan capture pruned")) {
        return false;
    }
    removed.reset(); // destroy the detached subtree; later events must not touch it
    const auto orphan_up =
        router.dispatch(root, pointer_for(InputEventType::PointerUp, 150, 150, 2));
    if (!expect(orphan_up.target == &root, "orphan up uses hit target") ||
        !expect(router.captured(2) == nullptr, "orphan up leaves no capture")) {
        return false;
    }

    // Only the primary pointer drives hover: a stray pointer id does not set it.
    router.reset();
    const auto stray = router.dispatch(root, pointer_for(InputEventType::PointerMove, 20, 20, 5));
    if (!expect(stray.target == &left, "non-primary move still dispatches") ||
        !expect(router.hovered() == nullptr, "non-primary move does not hover")) {
        return false;
    }

    // Explicit capture APIs route a pointer without a press; releasing one keeps the others.
    router.reset();
    if (!expect(router.set_pointer_capture(&left, 7), "explicit capture accepted") ||
        !expect(router.set_pointer_capture(&root, 8), "second explicit capture accepted") ||
        !expect(router.captured(7) == &left && router.captured(8) == &root,
                "explicit captures resolve") ||
        !expect(router.primary_pointer_id() == 0, "explicit capture does not set primary")) {
        return false;
    }
    router.release_pointer_capture(7);
    if (!expect(router.captured(7) == nullptr, "explicit release drops one capture") ||
        !expect(router.captured(8) == &root, "explicit release keeps the other")) {
        return false;
    }

    // The bounded map rejects extra pointers without leaking a capture for them.
    router.reset();
    for (std::uint32_t i = 0; i < InputRouter::kMaxPointerCaptures; ++i) {
        if (!expect(router.set_pointer_capture(&root, 20 + i), "bounded capture accepts")) {
            return false;
        }
    }
    if (!expect(!router.set_pointer_capture(&root, 99), "bounded capture rejects overflow") ||
        !expect(router.captured(99) == nullptr, "overflow pointer has no capture")) {
        return false;
    }
    router.release_pointer_capture(20);
    if (!expect(router.set_pointer_capture(&root, 99), "released slot is reusable") ||
        !expect(router.captured(99) == &root, "reused slot resolves")) {
        return false;
    }

    // PointerCancel ends only the cancelled pointer; a window blur clears every pointer.
    router.reset();
    router.dispatch(root, pointer_for(InputEventType::PointerDown, 20, 20, 3));
    router.dispatch(root, pointer_for(InputEventType::PointerDown, 150, 150, 4));
    router.dispatch(root, pointer_for(InputEventType::PointerCancel, 20, 20, 3));
    if (!expect(router.captured(3) == nullptr, "cancel one releases that pointer") ||
        !expect(router.captured(4) == &root, "cancel one keeps the other pointer") ||
        !expect(router.primary_pointer_id() == 4, "cancel of primary hands off") ||
        !expect(router.active() == nullptr, "cancel of primary clears active")) {
        return false;
    }
    const auto blur = router.dispatch(root, lifecycle(InputEventType::Blur));
    return expect(!blur.dispatched, "blur not dispatched") &&
           expect(router.captured(4) == nullptr, "blur clears every capture") &&
           expect(router.primary_pointer_id() == 0, "blur resets primary pointer") &&
           expect(router.hovered() == nullptr && router.active() == nullptr,
                  "blur clears hover and active");
}

} // namespace

int main() {
    Recorder root(1);
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<Recorder>(2);
    child->set_bounds({10, 10, 50, 50});
    child->set_focusable(true);
    Recorder& child_ref = static_cast<Recorder&>(root.add_child(std::move(child)));

    InputRouter router;
    const auto down = router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(down.dispatched, "down dispatched") ||
        !expect(down.target == &child_ref, "down target") ||
        !expect(router.captured() == &child_ref, "down captures") ||
        !expect(router.focus_manager().focused() == &child_ref, "down focuses target") ||
        !expect(child_ref.log.size() == 2 && child_ref.log[0] == "2:focus:target" &&
                    child_ref.log[1] == "2:down:target",
                "down child focus and pointer events") ||
        !expect(root.log.size() == 4 && root.log[0] == "1:focus:capture" &&
                    root.log[1] == "1:focus:bubble" && root.log[2] == "1:down:capture" &&
                    root.log[3] == "1:down:bubble",
                "down root focus and pointer capture/bubble")) {
        return 1;
    }

    child_ref.log.clear();
    root.log.clear();
    const auto move = router.dispatch(root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(move.dispatched, "captured move dispatched") ||
        !expect(move.target == &child_ref, "captured move target child") ||
        !expect(router.captured() == &child_ref, "move keeps capture") ||
        !expect(child_ref.log.size() == 1 && child_ref.log[0] == "2:move:target",
                "child receives move outside") ||
        !expect(root.log.size() == 2 && root.log[0] == "1:move:capture" &&
                    root.log[1] == "1:move:bubble",
                "root captures and bubbles move")) {
        return 2;
    }

    child_ref.log.clear();
    root.log.clear();
    const auto up = router.dispatch(root, pointer(InputEventType::PointerUp, 180, 180));
    if (!expect(up.dispatched, "captured up dispatched") ||
        !expect(up.target == &child_ref, "captured up target child") ||
        !expect(router.captured() == nullptr, "up releases capture") ||
        !expect(router.focus_manager().focused() == &child_ref, "up keeps focus") ||
        !expect(child_ref.log.size() == 1 && child_ref.log[0] == "2:up:target",
                "child receives up")) {
        return 3;
    }

    child_ref.log.clear();
    root.log.clear();
    const auto after_up = router.dispatch(root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(after_up.dispatched, "after up dispatched") ||
        !expect(after_up.target == &root, "after up hits root") ||
        !expect(root.log.size() == 3 && root.log[0] == "1:over:target" &&
                    root.log[1] == "1:enter:target" && root.log[2] == "1:move:target",
                "after up enters and targets root")) {
        return 4;
    }

    const auto root_down = router.dispatch(root, pointer(InputEventType::PointerDown, 150, 150));
    if (!expect(root_down.dispatched, "root down dispatched") ||
        !expect(root_down.target == &root, "root down target") ||
        !expect(router.captured() == &root, "root down captures") ||
        !expect(router.focus_manager().focused() == nullptr, "non-focusable down clears focus")) {
        return 5;
    }
    router.dispatch(root, pointer(InputEventType::PointerUp, 150, 150));

    const auto outside = router.dispatch(root, pointer(InputEventType::PointerDown, 500, 500));
    if (!expect(!outside.dispatched, "outside down ignored") ||
        !expect(router.captured() == nullptr, "outside no capture") ||
        !expect(router.focus_manager().focused() == nullptr, "outside clears focus")) {
        return 6;
    }

    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    const auto cancelled = router.dispatch(root, pointer(InputEventType::PointerCancel, 20, 20));
    if (!expect(!cancelled.dispatched, "cancel not dispatched") ||
        !expect(router.captured() == nullptr, "cancel releases capture") ||
        !expect(router.focus_manager().focused() == &child_ref, "cancel keeps focus")) {
        return 7;
    }

    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    child_ref.log.clear();
    root.log.clear();
    InputEvent key;
    key.type = InputEventType::KeyDown;
    key.key = 0x41;
    const auto keyed = router.dispatch(root, key);
    if (!expect(keyed.dispatched, "key routed to focus") ||
        !expect(keyed.type == EventType::KeyDown, "key type") ||
        !expect(keyed.target == &child_ref, "key target") ||
        !expect(router.captured() == &child_ref, "key keeps capture")) {
        return 8;
    }
    router.reset();
    if (!expect(router.captured() == nullptr, "reset releases capture") ||
        !expect(router.focus_manager().focused() == nullptr, "reset clears focus")) {
        return 9;
    }

    if (!interaction_state(root, child_ref, router)) {
        return 10;
    }

    if (!per_pointer_capture()) {
        return 11;
    }

    std::cout << "input_router: ok\n";
    return 0;
}
