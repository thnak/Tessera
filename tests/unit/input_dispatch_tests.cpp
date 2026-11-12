// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Dirty;
using tessera::ui::dispatch_input;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::has_flag;
using tessera::ui::InputDispatchResult;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::PointerKind;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_dispatch: " << message << "\n";
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
    case EventType::Click:
        return "click";
    case EventType::Scroll:
        return "scroll";
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
        last_event = event;
        log.push_back(std::to_string(id()) + ":" + type_name(event.type) + ":" +
                      phase_name(event.phase));
    }

    std::vector<std::string> log;
    Event last_event;
};

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

InputEvent kind_pointer(InputEventType type, float x, float y, std::uint32_t pointer_id,
                        PointerKind kind) {
    InputEvent event = pointer(type, x, y);
    event.pointer_id = pointer_id;
    event.pointer_kind = kind;
    return event;
}

// Pointer kinds: mouse/pen hover, touch only activates, and capture stays per pointer id.
bool pointer_kind_semantics() {
    Recorder root(20);
    root.set_bounds({0, 0, 200, 200});
    auto left_ptr = std::make_unique<Recorder>(21);
    left_ptr->set_bounds({10, 10, 50, 50});
    left_ptr->set_focusable(true);
    auto right_ptr = std::make_unique<Recorder>(22);
    right_ptr->set_bounds({100, 100, 50, 50});
    right_ptr->set_focusable(true);
    Recorder& left = static_cast<Recorder&>(root.add_child(std::move(left_ptr)));
    Recorder& right = static_cast<Recorder&>(root.add_child(std::move(right_ptr)));

    InputRouter router;

    // Mouse hover is unchanged: the primary mouse pointer drives it.
    const auto mouse_move = router.dispatch(
        root, kind_pointer(InputEventType::PointerMove, 20, 20, 0, PointerKind::Mouse));
    if (!expect(mouse_move.target == &left, "mouse move hits left") ||
        !expect(router.hovered() == &left, "mouse move hovers left") ||
        !expect(has_state(left.pseudo_state(), PseudoState::Hover), "mouse hover bit set") ||
        !expect(left.last_event.pointer_kind == PointerKind::Mouse, "mouse kind forwarded")) {
        return false;
    }
    router.dispatch(root,
                    kind_pointer(InputEventType::PointerMove, 500, 500, 0, PointerKind::Mouse));
    if (!expect(router.hovered() == nullptr, "mouse leave clears hover")) {
        return false;
    }

    // Touch contact: active, capture and focus, but no hover.
    const auto touch_down = router.dispatch(
        root, kind_pointer(InputEventType::PointerDown, 120, 120, 5, PointerKind::Touch));
    if (!expect(touch_down.target == &right, "touch down hits right") ||
        !expect(router.active() == &right, "touch press becomes active") ||
        !expect(has_state(right.pseudo_state(), PseudoState::Active), "touch active bit set") ||
        !expect(router.captured(5) == &right, "touch press captures pointer") ||
        !expect(router.focus_manager().focused() == &right, "touch press focuses target") ||
        !expect(!has_state(right.pseudo_state(), PseudoState::FocusVisible),
                "touch focus keeps :focus-visible clear") ||
        !expect(router.hovered() == nullptr, "touch press does not hover") ||
        !expect(!has_state(right.pseudo_state(), PseudoState::Hover),
                "touch target has no hover") ||
        !expect(right.last_event.pointer_kind == PointerKind::Touch, "touch kind forwarded")) {
        return false;
    }

    // Touch moves stay captured and never hover, even over another node.
    const auto touch_move = router.dispatch(
        root, kind_pointer(InputEventType::PointerMove, 20, 20, 5, PointerKind::Touch));
    if (!expect(touch_move.target == &right, "touch move stays captured") ||
        !expect(router.hovered() == nullptr, "touch move never hovers") ||
        !expect(!has_state(left.pseudo_state(), PseudoState::Hover), "touch move adds no hover") ||
        !expect(!has_state(right.pseudo_state(), PseudoState::Hover),
                "capture target has no hover")) {
        return false;
    }

    // Touch up clears active with no hover residue.
    const auto touch_up = router.dispatch(
        root, kind_pointer(InputEventType::PointerUp, 20, 20, 5, PointerKind::Touch));
    if (!expect(touch_up.target == &right, "touch up uses capture") ||
        !expect(router.active() == nullptr, "touch up clears active") ||
        !expect(!has_state(right.pseudo_state(), PseudoState::Active),
                "touch up clears active bit") ||
        !expect(router.captured(5) == nullptr, "touch up releases capture") ||
        !expect(router.hovered() == nullptr, "touch up leaves no hover")) {
        return false;
    }

    // Touch cancel also leaves no hover/active residue.
    router.dispatch(root,
                    kind_pointer(InputEventType::PointerDown, 120, 120, 6, PointerKind::Touch));
    const auto touch_cancel = router.dispatch(
        root, kind_pointer(InputEventType::PointerCancel, 120, 120, 6, PointerKind::Touch));
    if (!expect(!touch_cancel.dispatched, "touch cancel not dispatched") ||
        !expect(router.active() == nullptr, "touch cancel clears active") ||
        !expect(router.captured(6) == nullptr, "touch cancel releases capture") ||
        !expect(router.hovered() == nullptr, "touch cancel leaves no hover") ||
        !expect(!has_state(right.pseudo_state(), PseudoState::Hover), "no hover residue")) {
        return false;
    }

    // A secondary touch pointer never touches hover and keeps its own capture.
    router.dispatch(root, kind_pointer(InputEventType::PointerDown, 20, 20, 7, PointerKind::Touch));
    router.dispatch(root,
                    kind_pointer(InputEventType::PointerDown, 120, 120, 8, PointerKind::Touch));
    const auto secondary_move = router.dispatch(
        root, kind_pointer(InputEventType::PointerMove, 20, 20, 8, PointerKind::Touch));
    if (!expect(secondary_move.target == &right, "secondary touch stays captured") ||
        !expect(router.captured(7) == &left, "primary touch capture intact") ||
        !expect(router.captured(8) == &right, "secondary touch capture intact") ||
        !expect(router.hovered() == nullptr, "secondary touch never hovers") ||
        !expect(router.active() == &left, "secondary touch does not steal active")) {
        return false;
    }
    router.dispatch(root, kind_pointer(InputEventType::PointerUp, 20, 20, 7, PointerKind::Touch));
    if (!expect(router.captured(7) == nullptr, "primary touch released") ||
        !expect(router.captured(8) == &right, "releasing one touch keeps the other") ||
        !expect(router.active() == nullptr, "primary touch up clears active")) {
        return false;
    }
    router.dispatch(root, kind_pointer(InputEventType::PointerUp, 120, 120, 8, PointerKind::Touch));

    // Pen hover is allowed (pens report pre-contact positions): the primary pen pointer hovers,
    // and a pen press carries its pressure through to the handler.
    router.dispatch(root, kind_pointer(InputEventType::PointerMove, 20, 20, 0, PointerKind::Pen));
    if (!expect(router.hovered() == &left, "pen hover allowed") ||
        !expect(has_state(left.pseudo_state(), PseudoState::Hover), "pen hover bit set")) {
        return false;
    }
    InputEvent pen_down = kind_pointer(InputEventType::PointerDown, 20, 20, 0, PointerKind::Pen);
    pen_down.pressure = 0.75f;
    const auto pen_result = router.dispatch(root, pen_down);
    if (!expect(pen_result.target == &left, "pen down hits left") ||
        !expect(router.active() == &left, "pen press becomes active") ||
        !expect(router.captured(0) == &left, "pen press captures") ||
        !expect(left.last_event.pointer_kind == PointerKind::Pen, "pen kind forwarded") ||
        !expect(left.last_event.pressure == 0.75f, "pen pressure forwarded")) {
        return false;
    }
    router.dispatch(root, kind_pointer(InputEventType::PointerUp, 20, 20, 0, PointerKind::Pen));

    // Capture is keyed by pointer id only, so different ids stay independent across kinds.
    router.dispatch(root, kind_pointer(InputEventType::PointerDown, 20, 20, 0, PointerKind::Mouse));
    router.dispatch(root,
                    kind_pointer(InputEventType::PointerDown, 120, 120, 5, PointerKind::Touch));
    if (!expect(router.captured(0) == &left, "mouse capture present") ||
        !expect(router.captured(5) == &right, "touch capture alongside mouse")) {
        return false;
    }
    router.dispatch(root,
                    kind_pointer(InputEventType::PointerCancel, 120, 120, 5, PointerKind::Touch));
    if (!expect(router.captured(0) == &left, "touch cancel keeps the mouse capture") ||
        !expect(router.active() == &left, "touch cancel keeps the mouse active")) {
        return false;
    }
    router.dispatch(root, kind_pointer(InputEventType::PointerUp, 20, 20, 0, PointerKind::Mouse));
    return expect(router.captured(0) == nullptr, "mouse up releases its capture");
}

} // namespace

int main() {
    Recorder root(1);
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<Recorder>(2);
    child->set_bounds({10, 10, 50, 50});
    Recorder& child_ref = static_cast<Recorder&>(root.add_child(std::move(child)));

    const InputDispatchResult down =
        dispatch_input(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(down.dispatched, "pointer down dispatched") ||
        !expect(down.type == EventType::PointerDown, "pointer down type") ||
        !expect(down.target == &child_ref, "pointer down target") ||
        !expect(child_ref.log.size() == 1, "child target event") ||
        !expect(root.log.size() == 2, "capture and bubble root") ||
        !expect(root.log[0] == "1:down:capture", "capture phase") ||
        !expect(child_ref.log[0] == "2:down:target", "target phase") ||
        !expect(root.log[1] == "1:down:bubble", "bubble phase") ||
        !expect(child_ref.pseudo_state() == PseudoState::None, "no active pseudo-state") ||
        !expect(root.pseudo_state() == PseudoState::None, "no ancestor pseudo-state")) {
        return 1;
    }

    child_ref.log.clear();
    root.log.clear();
    const InputDispatchResult up = dispatch_input(root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(up.dispatched, "pointer up dispatched") ||
        !expect(up.type == EventType::PointerUp, "pointer up type") ||
        !expect(child_ref.log.size() == 1 && child_ref.log[0] == "2:up:target",
                "pointer up target")) {
        return 2;
    }

    child_ref.log.clear();
    root.log.clear();
    const InputDispatchResult move =
        dispatch_input(root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(move.dispatched, "pointer move dispatched") ||
        !expect(move.type == EventType::PointerMove, "pointer move type") ||
        !expect(move.target == &root, "pointer move target root") ||
        !expect(root.log.size() == 1 && root.log[0] == "1:move:target", "root target")) {
        return 3;
    }

    root.log.clear();
    const InputDispatchResult outside =
        dispatch_input(root, pointer(InputEventType::PointerDown, 500, 500));
    if (!expect(!outside.dispatched, "outside not dispatched") ||
        !expect(outside.target == nullptr, "outside target null") ||
        !expect(root.log.empty(), "outside no events")) {
        return 4;
    }

    // The free dispatch helper is stateless: only InputRouter owns hover/active pseudo-state.
    child_ref.set_visible(false);
    child_ref.clear_dirty(Dirty::All);
    root.clear_dirty(Dirty::All);
    root.log.clear();
    const InputDispatchResult hidden =
        dispatch_input(root, pointer(InputEventType::PointerMove, 20, 20));
    if (!expect(hidden.dispatched, "hidden child move dispatched") ||
        !expect(hidden.target == &root, "hidden child skipped by hit testing") ||
        !expect(root.pseudo_state() == PseudoState::None, "stateless move sets no hover") ||
        !expect(child_ref.pseudo_state() == PseudoState::None, "hidden child gets no state") ||
        !expect(!has_flag(root.dirty(), Dirty::Style), "stateless move marks no style dirty")) {
        return 5;
    }
    child_ref.set_visible(true);

    InputEvent key;
    key.type = InputEventType::KeyDown;
    key.x = 20;
    key.y = 20;
    const InputDispatchResult unsupported = dispatch_input(root, key);
    if (!expect(!unsupported.dispatched, "key ignored") ||
        !expect(unsupported.target == nullptr, "key target null")) {
        return 6;
    }

    const InputDispatchResult cancelled =
        dispatch_input(root, pointer(InputEventType::PointerCancel, 20, 20));
    if (!expect(!cancelled.dispatched, "cancel ignored") ||
        !expect(cancelled.target == nullptr, "cancel target null")) {
        return 7;
    }

    InputEvent free_pen = pointer(InputEventType::PointerDown, 20, 20);
    free_pen.pointer_kind = PointerKind::Pen;
    const InputDispatchResult free_pen_down = dispatch_input(root, free_pen);
    if (!expect(free_pen_down.dispatched, "free dispatch forwards pointer events") ||
        !expect(child_ref.last_event.pointer_kind == PointerKind::Pen,
                "free dispatch forwards pointer kind")) {
        return 8;
    }

    if (!pointer_kind_semantics()) {
        return 9;
    }

    std::cout << "input_dispatch: ok\n";
    return 0;
}
