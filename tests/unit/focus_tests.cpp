// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::NodeId;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "focus: " << message << "\n";
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
    case EventType::KeyDown:
        return "keydown";
    case EventType::KeyUp:
        return "keyup";
    case EventType::Focus:
        return "focus";
    case EventType::Blur:
        return "blur";
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
    Recorder(NodeId id, std::vector<std::string>& trace)
        : Node(id),
          trace(trace) {}

    void on_event(Event& event) override {
        trace.push_back(std::to_string(id()) + ":" + type_name(event.type) + ":" +
                        phase_name(event.phase));
        last_key = event.key;
        last_modifiers = event.modifiers;
        last_text_codepoint = event.text_codepoint;
        last_repeat = event.repeat;
        if (stop_enabled && event.type == stop_type && event.phase == EventPhase::Target) {
            event.stop_propagation();
        }
    }

    std::vector<std::string>& trace;
    bool stop_enabled = false;
    EventType stop_type = EventType::KeyDown;
    std::uint32_t last_key = 0;
    Modifier last_modifiers = Modifier::None;
    std::uint32_t last_text_codepoint = 0;
    bool last_repeat = false;
};

std::vector<std::string> target_events(const std::vector<std::string>& trace) {
    std::vector<std::string> result;
    for (const auto& entry : trace) {
        if (entry.ends_with(":target")) {
            result.push_back(entry);
        }
    }
    return result;
}

InputEvent key_event(InputEventType type) {
    InputEvent event;
    event.type = type;
    event.key = 0x41;
    event.modifiers = Modifier::Shift | Modifier::Control;
    event.text_codepoint = 0x61;
    event.repeat = true;
    return event;
}

} // namespace

int main() {
    std::vector<std::string> trace;

    Recorder root(1, trace);
    root.set_bounds({0, 0, 500, 500});

    auto panel_ptr = std::make_unique<Recorder>(2, trace);
    panel_ptr->set_bounds({0, 0, 200, 200});
    auto& panel = static_cast<Recorder&>(root.add_child(std::move(panel_ptr)));

    auto first_ptr = std::make_unique<Recorder>(3, trace);
    first_ptr->set_bounds({10, 10, 50, 50});
    first_ptr->set_focusable(true);
    auto& first = static_cast<Recorder&>(panel.add_child(std::move(first_ptr)));

    auto second_ptr = std::make_unique<Recorder>(4, trace);
    second_ptr->set_bounds({100, 10, 50, 50});
    second_ptr->set_focusable(true);
    auto& second = static_cast<Recorder&>(panel.add_child(std::move(second_ptr)));

    auto hidden_ptr = std::make_unique<Recorder>(5, trace);
    hidden_ptr->set_visible(false);
    auto& hidden = static_cast<Recorder&>(root.add_child(std::move(hidden_ptr)));

    auto hidden_child_ptr = std::make_unique<Recorder>(6, trace);
    hidden_child_ptr->set_focusable(true);
    auto& hidden_child = static_cast<Recorder&>(hidden.add_child(std::move(hidden_child_ptr)));

    auto plain_ptr = std::make_unique<Recorder>(7, trace);
    plain_ptr->set_bounds({10, 100, 80, 80});
    auto& plain = static_cast<Recorder&>(root.add_child(std::move(plain_ptr)));

    auto third_ptr = std::make_unique<Recorder>(8, trace);
    third_ptr->set_bounds({10, 160, 30, 30});
    third_ptr->set_focusable(true);
    auto& third = static_cast<Recorder&>(plain.add_child(std::move(third_ptr)));

    FocusManager manager;
    manager.set_root(root);
    if (!expect(manager.focused() == nullptr, "new manager has no focus") ||
        !expect(!root.focusable(), "nodes are not focusable by default")) {
        return 1;
    }

    trace.clear();
    if (!expect(manager.request_focus(&first), "focus first node") ||
        !expect(manager.focused() == &first, "first node focused") ||
        !expect(target_events(trace) == std::vector<std::string>{"3:focus:target"},
                "focus event on target")) {
        return 2;
    }

    trace.clear();
    if (!expect(manager.request_focus(&first), "re-focus same node") ||
        !expect(trace.empty(), "re-focus emits no events")) {
        return 3;
    }

    trace.clear();
    if (!expect(manager.request_focus(&second), "focus second node") ||
        !expect(manager.focused() == &second, "second node focused") ||
        !expect(target_events(trace) == std::vector<std::string>{"3:blur:target", "4:focus:target"},
                "blur precedes focus")) {
        return 4;
    }

    trace.clear();
    manager.clear_focus();
    if (!expect(manager.focused() == nullptr, "clear focus") ||
        !expect(target_events(trace) == std::vector<std::string>{"4:blur:target"},
                "clear dispatches blur")) {
        return 5;
    }

    trace.clear();
    manager.clear_focus();
    if (!expect(trace.empty(), "clear without focus emits no events")) {
        return 6;
    }

    Recorder detached(50, trace);
    detached.set_focusable(true);
    first.set_visible(false);
    if (!expect(!manager.request_focus(nullptr), "null focus rejected") ||
        !expect(!manager.request_focus(&plain), "non-focusable focus rejected") ||
        !expect(!manager.request_focus(&first), "invisible focus rejected") ||
        !expect(!manager.request_focus(&hidden_child), "invisible ancestor focus rejected") ||
        !expect(!manager.request_focus(&detached), "detached focus rejected") ||
        !expect(manager.focused() == nullptr, "rejected focus keeps no focus")) {
        return 7;
    }
    first.set_visible(true);

    trace.clear();
    if (!expect(manager.focus_next(root), "next focuses first node") ||
        !expect(manager.focused() == &first, "next first") ||
        !expect(manager.focus_next(root), "next second") ||
        !expect(manager.focused() == &second, "second in tab order") ||
        !expect(manager.focus_next(root), "next third") ||
        !expect(manager.focused() == &third, "third in tab order") ||
        !expect(manager.focus_next(root), "next wraps") ||
        !expect(manager.focused() == &first, "wrap to first") ||
        !expect(manager.focus_previous(root), "previous wraps") ||
        !expect(manager.focused() == &third, "wrap to last") ||
        !expect(manager.focus_previous(root), "previous second") ||
        !expect(manager.focused() == &second, "previous in tab order")) {
        return 8;
    }

    InputRouter router;
    router.focus_manager().set_root(root);
    if (!expect(router.focus_manager().request_focus(&first), "router focus first")) {
        return 9;
    }

    trace.clear();
    const InputEvent key_down = key_event(InputEventType::KeyDown);
    const auto key_result = router.dispatch(root, key_down);
    if (!expect(key_result.dispatched, "key down dispatched") ||
        !expect(key_result.type == EventType::KeyDown, "key down type") ||
        !expect(key_result.target == &first, "key down target") ||
        !expect(trace == std::vector<std::string>{"1:keydown:capture", "2:keydown:capture",
                                                  "3:keydown:target", "2:keydown:bubble",
                                                  "1:keydown:bubble"},
                "key down capture/target/bubble") ||
        !expect(first.last_key == 0x41, "key seeded") ||
        !expect(first.last_modifiers == (Modifier::Shift | Modifier::Control),
                "modifiers seeded") ||
        !expect(first.last_text_codepoint == 0x61, "text codepoint seeded") ||
        !expect(first.last_repeat, "repeat seeded")) {
        return 10;
    }

    trace.clear();
    const auto up_result = router.dispatch(root, key_event(InputEventType::KeyUp));
    if (!expect(up_result.dispatched, "key up dispatched") ||
        !expect(up_result.type == EventType::KeyUp, "key up type") ||
        !expect(up_result.target == &first, "key up target") ||
        !expect(target_events(trace) == std::vector<std::string>{"3:keyup:target"},
                "key up target event")) {
        return 11;
    }

    first.stop_enabled = true;
    trace.clear();
    const auto stopped = router.dispatch(root, key_down);
    first.stop_enabled = false;
    if (!expect(stopped.dispatched, "stopped key still dispatched") ||
        !expect(trace == std::vector<std::string>{"1:keydown:capture", "2:keydown:capture",
                                                  "3:keydown:target"},
                "key stop propagation")) {
        return 12;
    }

    router.focus_manager().clear_focus();
    trace.clear();
    const auto unfocused = router.dispatch(root, key_down);
    if (!expect(!unfocused.dispatched, "key without focus ignored") ||
        !expect(unfocused.target == nullptr, "key without focus has no target") ||
        !expect(trace.empty(), "key without focus emits no events")) {
        return 13;
    }

    std::cout << "focus: ok\n";
    return 0;
}
