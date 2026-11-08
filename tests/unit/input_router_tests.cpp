// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;

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

    InputRouter router;
    const auto down = router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    if (!expect(down.dispatched, "down dispatched") ||
        !expect(down.target == &child_ref, "down target") ||
        !expect(router.captured() == &child_ref, "down captures") ||
        !expect(child_ref.log.size() == 1, "down child event") ||
        !expect(root.log.size() == 2, "down root capture/bubble")) {
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
        !expect(child_ref.log.size() == 1 && child_ref.log[0] == "2:up:target",
                "child receives up")) {
        return 3;
    }

    child_ref.log.clear();
    root.log.clear();
    const auto after_up = router.dispatch(root, pointer(InputEventType::PointerMove, 150, 150));
    if (!expect(after_up.dispatched, "after up dispatched") ||
        !expect(after_up.target == &root, "after up hits root") ||
        !expect(root.log.size() == 1 && root.log[0] == "1:move:target", "after up root target")) {
        return 4;
    }

    const auto outside = router.dispatch(root, pointer(InputEventType::PointerDown, 500, 500));
    if (!expect(!outside.dispatched, "outside down ignored") ||
        !expect(router.captured() == nullptr, "outside no capture")) {
        return 5;
    }

    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    const auto cancelled = router.dispatch(root, pointer(InputEventType::PointerCancel, 20, 20));
    if (!expect(!cancelled.dispatched, "cancel not dispatched") ||
        !expect(router.captured() == nullptr, "cancel releases capture")) {
        return 6;
    }

    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    InputEvent key;
    key.type = InputEventType::KeyDown;
    const auto unsupported = router.dispatch(root, key);
    if (!expect(!unsupported.dispatched, "key ignored") ||
        !expect(router.captured() == &child_ref, "key keeps capture")) {
        return 7;
    }
    router.reset();
    if (!expect(router.captured() == nullptr, "reset releases capture")) {
        return 8;
    }

    std::cout << "input_router: ok\n";
    return 0;
}
