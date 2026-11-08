// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::dispatch_input;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputDispatchResult;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Node;

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
        !expect(root.log[1] == "1:down:bubble", "bubble phase")) {
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

    InputEvent key;
    key.type = InputEventType::KeyDown;
    key.x = 20;
    key.y = 20;
    const InputDispatchResult unsupported = dispatch_input(root, key);
    if (!expect(!unsupported.dispatched, "key ignored") ||
        !expect(unsupported.target == nullptr, "key target null")) {
        return 5;
    }

    const InputDispatchResult cancelled =
        dispatch_input(root, pointer(InputEventType::PointerCancel, 20, 20));
    if (!expect(!cancelled.dispatched, "cancel ignored") ||
        !expect(cancelled.target == nullptr, "cancel target null")) {
        return 6;
    }

    std::cout << "input_dispatch: ok\n";
    return 0;
}
