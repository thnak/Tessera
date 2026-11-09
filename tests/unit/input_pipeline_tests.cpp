// SPDX-License-Identifier: MIT

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <iostream>
#include <memory>
#include <vector>

namespace {

using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputPump;
using tessera::ui::InputPumpConfig;
using tessera::ui::InputPumpStats;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::PointerButton;
using tessera::ui::key::kTab;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_pipeline: " << message << "\n";
    }
    return condition;
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        events.push_back(event);
        if (prevent_tab && event.type == EventType::KeyDown && event.key == kTab) {
            event.prevent_default = true;
        }
    }

    std::vector<Event> events;
    bool prevent_tab = false;
};

struct Scene {
    Scene() {
        root.set_bounds({0, 0, 200, 200});

        auto child_ptr = std::make_unique<Recorder>(2);
        child = child_ptr.get();
        child->set_bounds({10, 10, 50, 50});
        child->set_focusable(true);
        root.add_child(std::move(child_ptr));

        auto other_ptr = std::make_unique<Recorder>(3);
        other = other_ptr.get();
        other->set_bounds({100, 100, 50, 50});
        other->set_focusable(true);
        root.add_child(std::move(other_ptr));
    }

    Recorder root{1};
    Recorder* child = nullptr;
    Recorder* other = nullptr;
};

void clear(Scene& scene) {
    scene.root.events.clear();
    scene.child->events.clear();
    scene.other->events.clear();
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

InputEvent key(InputEventType type, tessera::ui::KeyCode code,
               Modifier modifiers = Modifier::None) {
    InputEvent event;
    event.type = type;
    event.key = code;
    event.modifiers = modifiers;
    return event;
}

bool payload_mapping(Scene& scene, InputRouter& router) {
    InputEvent down = pointer(InputEventType::PointerDown, 20, 20);
    down.button = PointerButton::Right;
    down.pointer_id = 7;
    down.pressure = 0.75f;
    down.timestamp_s = 12.5;
    down.is_primary = true;
    down.modifiers = Modifier::Shift;

    const auto result = router.dispatch(scene.root, down);
    if (result.dispatched == false || result.target != scene.child) {
        return expect(false, "payload down dispatched to child");
    }
    const Event& target = scene.child->events.back();
    return expect(target.type == EventType::PointerDown, "payload down type") &&
           expect(target.x == 20 && target.y == 20, "payload coordinates") &&
           expect(target.button == PointerButton::Right, "payload button") &&
           expect(target.pointer_id == 7, "payload pointer id") &&
           expect(target.pressure == 0.75f, "payload pressure") &&
           expect(target.timestamp_s == 12.5, "payload timestamp") &&
           expect(target.is_primary, "payload primary flag") &&
           expect(target.modifiers == Modifier::Shift, "payload modifiers") &&
           expect(target.phase == tessera::ui::EventPhase::Target, "payload target phase") &&
           expect(scene.root.events.back().type == EventType::PointerDown,
                  "payload bubbles to root");
}

bool wheel_routing(Scene& scene, InputRouter& router) {
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    clear(scene);

    InputEvent wheel = pointer(InputEventType::Wheel, 20, 20);
    wheel.pointer_id = 9;
    wheel.dx = 0.5f;
    wheel.dy = -1.5f;
    const auto result = router.dispatch(scene.root, wheel);
    if (!expect(result.dispatched, "wheel dispatched") ||
        !expect(result.type == EventType::Wheel, "wheel type") ||
        !expect(result.target == scene.child, "wheel hit target")) {
        return false;
    }
    const Event& target = scene.child->events.back();
    if (!expect(target.type == EventType::Wheel, "wheel reaches target") ||
        !expect(target.scroll_x == 0.5f && target.scroll_y == -1.5f, "wheel deltas") ||
        !expect(target.pointer_id == 9, "wheel pointer id")) {
        return false;
    }
    if (!expect(scene.root.events.size() == 2, "wheel capture and bubble") ||
        !expect(scene.root.events[0].phase == tessera::ui::EventPhase::Capture, "wheel capture") ||
        !expect(scene.root.events[1].phase == tessera::ui::EventPhase::Bubble, "wheel bubble")) {
        return false;
    }

    const auto outside = router.dispatch(scene.root, pointer(InputEventType::Wheel, 500, 500));
    return expect(!outside.dispatched, "wheel outside not dispatched") &&
           expect(outside.target == nullptr, "wheel outside target null");
}

bool text_to_focus(Scene& scene, InputRouter& router) {
    router.reset();
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    clear(scene);

    InputEvent text;
    text.type = InputEventType::Text;
    text.text_codepoint = 'A';
    text.x = 150;
    text.y = 150; // over root, not over the focused child
    const auto result = router.dispatch(scene.root, text);
    if (!expect(result.dispatched, "text dispatched") ||
        !expect(result.type == EventType::Text, "text type") ||
        !expect(result.target == scene.child, "text goes to focused node")) {
        return false;
    }
    if (!expect(scene.child->events.back().text_codepoint == 'A', "text codepoint") ||
        !expect(scene.other->events.empty(), "text not hit-tested")) {
        return false;
    }

    router.reset();
    const auto unfocused = router.dispatch(scene.root, text);
    return expect(!unfocused.dispatched, "text without focus not dispatched") &&
           expect(unfocused.target == nullptr, "text without focus target null");
}

bool blur_cancels_capture(Scene& scene, InputRouter& router) {
    router.reset();
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(router.captured() == scene.child, "capture before blur")) {
        return false;
    }

    InputEvent blur;
    blur.type = InputEventType::Blur;
    const auto blurred = router.dispatch(scene.root, blur);
    if (!expect(!blurred.dispatched, "blur input not dispatched") ||
        !expect(router.captured() == nullptr, "blur releases capture") ||
        !expect(router.focus_manager().focused() == scene.child,
                "blur input does not clear scene focus")) {
        return false;
    }

    clear(scene);
    const auto move = router.dispatch(scene.root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(move.target == &scene.root, "move after blur hits root") ||
        !expect(scene.child->events.empty(), "move after blur not routed to old capture")) {
        return false;
    }

    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    InputEvent focus;
    focus.type = InputEventType::Focus;
    router.dispatch(scene.root, focus);
    return expect(router.captured() == nullptr, "focus input releases capture");
}

bool tab_traversal(Scene& scene, InputRouter& router) {
    router.reset();
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(router.focus_manager().focused() == scene.child, "tab starts on child")) {
        return false;
    }

    clear(scene);
    const auto forward = router.dispatch(scene.root, key(InputEventType::KeyDown, kTab));
    if (!expect(forward.dispatched, "tab dispatched") ||
        !expect(forward.focus_moved, "tab moved focus") ||
        !expect(router.focus_manager().focused() == scene.other, "tab moves to next node")) {
        return false;
    }

    const auto backward =
        router.dispatch(scene.root, key(InputEventType::KeyDown, kTab, Modifier::Shift));
    if (!expect(backward.focus_moved, "shift-tab moved focus") ||
        !expect(router.focus_manager().focused() == scene.child, "shift-tab moves back")) {
        return false;
    }

    scene.child->prevent_tab = true;
    const auto prevented = router.dispatch(scene.root, key(InputEventType::KeyDown, kTab));
    scene.child->prevent_tab = false;
    if (!expect(prevented.default_prevented, "prevent_default reported") ||
        !expect(!prevented.focus_moved, "prevent_default suppresses tab") ||
        !expect(router.focus_manager().focused() == scene.child, "prevent_default keeps focus")) {
        return false;
    }

    router.reset();
    const auto no_focus = router.dispatch(scene.root, key(InputEventType::KeyDown, kTab));
    if (!expect(!no_focus.dispatched, "tab without focus dispatches no key event") ||
        !expect(no_focus.focus_moved, "tab without focus starts traversal") ||
        !expect(router.focus_manager().focused() == scene.child,
                "tab without focus focuses the first candidate")) {
        return false;
    }

    router.reset();
    const auto no_focus_back =
        router.dispatch(scene.root, key(InputEventType::KeyDown, kTab, Modifier::Shift));
    if (!expect(no_focus_back.focus_moved, "shift-tab without focus starts traversal") ||
        !expect(router.focus_manager().focused() == scene.other,
                "shift-tab without focus focuses the last candidate")) {
        return false;
    }

    // A modal trap restricts traversal even when nothing is focused yet.
    router.reset();
    router.focus_manager().set_root(scene.root);
    router.focus_manager().set_modal_root(scene.other);
    router.focus_manager().clear_focus();
    const auto trapped = router.dispatch(scene.root, key(InputEventType::KeyDown, kTab));
    return expect(trapped.focus_moved, "tab without focus moves inside the modal trap") &&
           expect(router.focus_manager().focused() == scene.other,
                  "tab without focus picks the first modal candidate");
}

bool interaction_states(Scene& scene) {
    // Start from a clean scene so bits owned by the shared router in earlier cases cannot leak in.
    scene.root.set_pseudo_state(PseudoState::None);
    scene.child->set_pseudo_state(PseudoState::None);
    scene.other->set_pseudo_state(PseudoState::None);
    scene.root.clear_dirty(Dirty::All);
    scene.child->clear_dirty(Dirty::All);
    scene.other->clear_dirty(Dirty::All);

    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;

    pump.queue().push(pointer(InputEventType::PointerMove, 20, 20));
    pump.pump(scene.root, loop);
    if (!expect(pump.router().hovered() == scene.child, "pump move hovers child") ||
        !expect(has_state(scene.child->pseudo_state(), PseudoState::Hover),
                "pump move sets hover bit") ||
        !expect(tessera::ui::has_flag(scene.child->dirty(), Dirty::Style),
                "pump hover marks style dirty")) {
        return false;
    }

    // Moving to another target clears the previous hover target.
    pump.queue().push(pointer(InputEventType::PointerMove, 120, 120));
    pump.pump(scene.root, loop);
    if (!expect(pump.router().hovered() == scene.other, "pump move re-hovers") ||
        !expect(!has_state(scene.child->pseudo_state(), PseudoState::Hover),
                "pump move clears previous hover")) {
        return false;
    }

    // Press activates; release deactivates.
    pump.queue().push(pointer(InputEventType::PointerDown, 120, 120));
    pump.pump(scene.root, loop);
    if (!expect(pump.router().active() == scene.other, "pump press activates target") ||
        !expect(has_state(scene.other->pseudo_state(), PseudoState::Active),
                "pump press sets active bit")) {
        return false;
    }
    pump.queue().push(pointer(InputEventType::PointerUp, 120, 120));
    pump.pump(scene.root, loop);
    if (!expect(pump.router().active() == nullptr, "pump release clears active") ||
        !expect(!has_state(scene.other->pseudo_state(), PseudoState::Active),
                "pump release clears active bit")) {
        return false;
    }

    // Window blur input resets capture, hover, and active but not scene focus.
    pump.queue().push(pointer(InputEventType::PointerMove, 20, 20));
    pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));
    pump.pump(scene.root, loop);
    InputEvent blur;
    blur.type = InputEventType::Blur;
    pump.queue().push(blur);
    pump.pump(scene.root, loop);
    if (!expect(pump.router().captured() == nullptr, "pump blur clears capture") ||
        !expect(pump.router().hovered() == nullptr, "pump blur clears hover") ||
        !expect(pump.router().active() == nullptr, "pump blur clears active") ||
        !expect(pump.router().focus_manager().focused() == scene.child,
                "pump blur keeps scene focus")) {
        return false;
    }

    // Focus input resets the same interaction state.
    pump.queue().push(pointer(InputEventType::PointerMove, 20, 20));
    pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));
    pump.pump(scene.root, loop);
    InputEvent focus;
    focus.type = InputEventType::Focus;
    pump.queue().push(focus);
    pump.pump(scene.root, loop);
    pump.reset();
    return expect(pump.router().hovered() == nullptr && pump.router().active() == nullptr,
                  "pump focus input and reset clear interaction state");
}

bool pump_drain(Scene& scene) {
    ManualClock clock;
    RuntimeLoop loop(clock);
    if (!expect(!loop.should_render(), "loop idle before input")) {
        return false;
    }

    InputPump pump;
    pump.queue().push(pointer(InputEventType::PointerMove, 5, 5));
    pump.queue().push(pointer(InputEventType::PointerMove, 6, 6));
    pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));

    const InputPumpStats first = pump.pump(scene.root, loop);
    if (!expect(first.consumed == 2, "pump coalesces adjacent moves") ||
        !expect(first.dispatched == 2, "pump dispatches coalesced events") ||
        !expect(first.dropped == 0, "pump drops nothing") ||
        !expect(loop.should_render(), "pump requests a frame")) {
        return false;
    }

    loop.tick();
    if (!expect(!loop.should_render(), "tick clears the frame request")) {
        return false;
    }

    const InputPumpStats empty = pump.pump(scene.root, loop);
    if (!expect(empty.consumed == 0, "empty pump consumes nothing") ||
        !expect(!loop.should_render(), "empty pump does not wake the loop")) {
        return false;
    }

    // Drop focus and capture so the next batch exercises the "nothing to deliver" path.
    pump.reset();
    pump.queue().push(pointer(InputEventType::PointerUp, 20, 20));
    InputEvent text;
    text.type = InputEventType::Text;
    text.text_codepoint = 'x';
    pump.queue().push(text);
    const InputPumpStats dropped = pump.pump(scene.root, loop);
    if (!expect(dropped.consumed == 2, "dropped batch consumed") ||
        !expect(dropped.dispatched == 1, "pointer up dispatched") ||
        !expect(dropped.dropped == 1, "text without focus dropped") ||
        !expect(loop.should_render(), "dropped event still wakes the loop")) {
        return false;
    }

    const InputPumpStats total = pump.total();
    return expect(total.consumed == 4, "pump totals consumed") &&
           expect(total.dispatched == 3, "pump totals dispatched") &&
           expect(total.dropped == 1, "pump totals dropped");
}

bool pump_budget(Scene& scene) {
    InputPumpConfig config;
    config.queue_capacity = 8;
    config.max_events_per_tick = 2;
    InputPump pump(config);

    for (int i = 0; i < 5; ++i) {
        pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));
        pump.queue().push(pointer(InputEventType::PointerUp, 20, 20));
    }
    if (!expect(pump.queue().size() == 8, "budget queue filled")) {
        return false;
    }

    ManualClock clock;
    RuntimeLoop loop(clock);
    const InputPumpStats first = pump.pump(scene.root, loop);
    if (!expect(first.consumed == 2, "budget bounds the first pump") ||
        !expect(pump.queue().size() == 6, "remaining events stay queued")) {
        return false;
    }

    while (!pump.queue().empty()) {
        pump.pump(scene.root, loop);
    }
    return expect(pump.total().consumed == 8, "all queued events eventually consumed");
}

bool pump_overflow(Scene& scene) {
    InputPumpConfig config;
    config.queue_capacity = 2;
    InputPump pump(config);

    const bool first = pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));
    const bool second = pump.queue().push(pointer(InputEventType::PointerUp, 20, 20));
    const bool third = pump.queue().push(pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(first && second, "queue accepts up to capacity") ||
        !expect(!third, "queue rejects beyond capacity") ||
        !expect(pump.queue().overflowed(), "queue overflow flag set")) {
        return false;
    }

    ManualClock clock;
    RuntimeLoop loop(clock);
    const InputPumpStats stats = pump.pump(scene.root, loop);
    if (!expect(stats.overflowed, "pump reports overflow") ||
        !expect(stats.consumed == 2, "pump drained the bounded queue")) {
        return false;
    }

    pump.reset();
    return expect(!pump.queue().overflowed(), "reset clears overflow") &&
           expect(pump.queue().empty(), "reset drains the queue") &&
           expect(pump.router().captured() == nullptr, "reset clears capture");
}

} // namespace

int main() {
    Scene scene;
    InputRouter router;

    if (!payload_mapping(scene, router)) {
        return 1;
    }
    if (!wheel_routing(scene, router)) {
        return 2;
    }
    if (!text_to_focus(scene, router)) {
        return 3;
    }
    if (!blur_cancels_capture(scene, router)) {
        return 4;
    }
    if (!tab_traversal(scene, router)) {
        return 5;
    }
    if (!interaction_states(scene)) {
        return 6;
    }
    if (!pump_drain(scene)) {
        return 7;
    }
    if (!pump_budget(scene)) {
        return 8;
    }
    if (!pump_overflow(scene)) {
        return 9;
    }

    std::cout << "input_pipeline: ok\n";
    return 0;
}
