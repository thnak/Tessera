// SPDX-License-Identifier: MIT
//
// Gesture-pan scrolling through InputPump: a Drag claim whose press target has a scroll container
// scrolls it by the per-move delta (touch and mouse), clamps on every move, keeps its claim at a
// clamped edge, stops on cancel, leaves non-scrollable targets alone, prefers the innermost
// container, and is deterministic.

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/gesture.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Event;
using tessera::ui::GestureEvent;
using tessera::ui::GestureKind;
using tessera::ui::GesturePhase;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputPump;
using tessera::ui::Node;
using tessera::ui::PointerKind;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scroll_pan: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 0.001f;
}

// Slow event timestamps keep the built-in Swipe recognizer below its 500 px/s claim threshold, so
// these scripts exercise the Drag recognizer deterministically (the arena's claim order is fixed).
InputEvent pointer_event(InputEventType type, float x, float y, double timestamp_s,
                         std::uint32_t pointer_id, PointerKind kind) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.timestamp_s = timestamp_s;
    event.pointer_id = pointer_id;
    event.pointer_kind = kind;
    event.is_primary = true;
    return event;
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        ++events;
        last_type = event.type;
    }

    int events = 0;
    tessera::ui::EventType last_type = tessera::ui::EventType::PointerDown;
};

// Root 200x200 with a scrollable 100x100 container (content 100x300) holding a hit target at
// world (20,20)..(40,40).
struct PanScene {
    PanScene() {
        root.set_bounds({0, 0, 200, 200});
        auto container_owner = std::make_unique<Node>(2);
        container = container_owner.get();
        container->set_bounds({10, 10, 100, 100});
        container->set_scrollable_y(true);
        container->set_content_size(100.0f, 300.0f);
        root.add_child(std::move(container_owner));

        auto child_owner = std::make_unique<Recorder>(3);
        child = child_owner.get();
        child->set_bounds({10, 10, 20, 20});
        container->add_child(std::move(child_owner));
    }

    Recorder root{1};
    Node* container = nullptr;
    Recorder* child = nullptr;
};

struct Harness {
    Harness() {
        pump.enable_gestures();
        pump.set_gesture_callback(&collect, &gestures);
    }

    static void collect(void* user, const GestureEvent& event) {
        static_cast<std::vector<GestureEvent>*>(user)->push_back(event);
    }

    void feed(Node& root, const InputEvent& event) {
        pump.queue().push(event);
        pump.pump(root, loop);
    }

    [[nodiscard]] std::size_t drag_events(GesturePhase phase) const {
        std::size_t count = 0;
        for (const GestureEvent& event : gestures) {
            if (event.kind == GestureKind::Drag && event.phase == phase) {
                ++count;
            }
        }
        return count;
    }

    ManualClock clock;
    RuntimeLoop loop{clock};
    InputPump pump;
    std::vector<GestureEvent> gestures;
};

bool drag_scrolls_touch_and_mouse() {
    for (const PointerKind kind : {PointerKind::Touch, PointerKind::Mouse}) {
        PanScene scene;
        Harness harness;
        const std::uint32_t id = kind == PointerKind::Touch ? 7U : 1U;
        const auto down = [&](float x, float y, double t) {
            return pointer_event(InputEventType::PointerDown, x, y, t, id, kind);
        };
        const auto move = [&](float x, float y, double t) {
            return pointer_event(InputEventType::PointerMove, x, y, t, id, kind);
        };

        harness.feed(scene.root, down(25.0f, 25.0f, 0.0));
        harness.feed(scene.root, move(25.0f, 15.0f, 0.1));
        if (!expect(near(scene.container->scroll_y(), 0.0f),
                    "the claim slop itself scrolls nothing")) {
            return false;
        }
        harness.feed(scene.root, move(25.0f, 5.0f, 0.2));
        if (!expect(near(scene.container->scroll_y(), 10.0f),
                    "a move scrolls by the per-move delta")) {
            return false;
        }
        harness.feed(scene.root, move(25.0f, -5.0f, 0.3));
        if (!expect(near(scene.container->scroll_y(), 20.0f), "deltas accumulate")) {
            return false;
        }
        harness.feed(scene.root,
                     pointer_event(InputEventType::PointerUp, 25.0f, -5.0f, 0.4, id, kind));
        if (!expect(near(scene.container->scroll_y(), 20.0f), "release keeps the offset")) {
            return false;
        }
        if (!expect(harness.drag_events(GesturePhase::Began) == 1,
                    "one Drag Began reaches the callback") ||
            !expect(harness.drag_events(GesturePhase::Ended) == 1,
                    "one Drag Ended reaches the callback")) {
            return false;
        }
    }
    return true;
}

bool clamps_on_every_move_and_keeps_claiming() {
    PanScene scene;
    Harness harness;
    tessera::ui::scroll_to(*scene.container, 0.0f, 100.0f);

    harness.feed(scene.root, pointer_event(InputEventType::PointerDown, 25.0f, 25.0f, 0.0, 1,
                                           PointerKind::Touch));
    // Drag down: content follows the pointer, so the offset decreases toward the top clamp.
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 85.0f, 0.2, 1,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 145.0f, 0.4, 1,
                                           PointerKind::Touch));
    if (!expect(near(scene.container->scroll_y(), 40.0f), "mid-drag offset")) {
        return false;
    }
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 205.0f, 0.6, 1,
                                           PointerKind::Touch));
    if (!expect(near(scene.container->scroll_y(), 0.0f), "clamped at the top edge")) {
        return false;
    }
    const std::size_t changed_at_edge = harness.drag_events(GesturePhase::Changed);
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 265.0f, 0.8, 1,
                                           PointerKind::Touch));
    if (!expect(near(scene.container->scroll_y(), 0.0f), "stays clamped")) {
        return false;
    }
    if (!expect(harness.drag_events(GesturePhase::Changed) == changed_at_edge + 1,
                "the gesture keeps its claim at the edge (no cancel/chaining)")) {
        return false;
    }

    // The other edge: dragging up past the maximum clamps too.
    tessera::ui::scroll_to(*scene.container, 0.0f, 200.0f);
    harness.feed(scene.root, pointer_event(InputEventType::PointerUp, 25.0f, 265.0f, 1.0, 1,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerDown, 25.0f, 25.0f, 1.2, 2,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 5.0f, 1.4, 2,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, -55.0f, 1.6, 2,
                                           PointerKind::Touch));
    return expect(near(scene.container->scroll_y(), 200.0f), "clamped at the bottom edge");
}

bool cancel_stops_the_pan() {
    PanScene scene;
    Harness harness;
    harness.feed(scene.root, pointer_event(InputEventType::PointerDown, 25.0f, 25.0f, 0.0, 3,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 15.0f, 0.1, 3,
                                           PointerKind::Touch));
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 5.0f, 0.2, 3,
                                           PointerKind::Touch));
    if (!expect(near(scene.container->scroll_y(), 10.0f), "scrolled before the cancel")) {
        return false;
    }
    harness.feed(scene.root, pointer_event(InputEventType::PointerCancel, 25.0f, 5.0f, 0.3, 3,
                                           PointerKind::Touch));
    if (!expect(harness.drag_events(GesturePhase::Canceled) == 1, "cancel reaches the callback")) {
        return false;
    }
    harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, -95.0f, 0.4, 3,
                                           PointerKind::Touch));
    return expect(near(scene.container->scroll_y(), 10.0f), "moves after a cancel do not scroll");
}

bool non_scrollable_leaves_the_gesture_alone() {
    Recorder root(1);
    root.set_bounds({0, 0, 200, 200});
    auto target_owner = std::make_unique<Recorder>(2);
    Recorder* target = target_owner.get();
    target->set_bounds({10, 10, 100, 100});
    root.add_child(std::move(target_owner));

    Harness harness;
    harness.feed(
        root, pointer_event(InputEventType::PointerDown, 50.0f, 50.0f, 0.0, 4, PointerKind::Touch));
    harness.feed(
        root, pointer_event(InputEventType::PointerMove, 50.0f, 20.0f, 0.1, 4, PointerKind::Touch));
    harness.feed(root, pointer_event(InputEventType::PointerMove, 50.0f, -10.0f, 0.2, 4,
                                     PointerKind::Touch));
    harness.feed(
        root, pointer_event(InputEventType::PointerUp, 50.0f, -10.0f, 0.3, 4, PointerKind::Touch));

    // Nothing to scroll, but the Drag events and the pointer events both still flow: the scroll
    // handoff declines the gesture instead of consuming it.
    return expect(near(root.scroll_y(), 0.0f), "a non-scrollable chain never scrolls") &&
           expect(harness.drag_events(GesturePhase::Began) == 1,
                  "the drag still reaches the gesture callback") &&
           expect(target->events >= 3, "pointer events still reach the scene") &&
           expect(tessera::ui::scroll_target_for_id(root, target->id()) == nullptr,
                  "no scroll target for a non-scrollable subtree");
}

bool innermost_container_consumes() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto outer_owner = std::make_unique<Node>(2);
    Node& outer = root.add_child(std::move(outer_owner));
    outer.set_bounds({0, 0, 200, 200});
    outer.set_scrollable_y(true);
    outer.set_content_size(200.0f, 500.0f);
    auto inner_owner = std::make_unique<Node>(3);
    Node& inner = outer.add_child(std::move(inner_owner));
    inner.set_bounds({10, 10, 100, 100});
    inner.set_scrollable_y(true);
    inner.set_content_size(100.0f, 300.0f);
    auto child_owner = std::make_unique<Recorder>(4);
    inner.add_child(std::move(child_owner));

    Harness harness;
    harness.feed(
        root, pointer_event(InputEventType::PointerDown, 30.0f, 30.0f, 0.0, 5, PointerKind::Touch));
    harness.feed(
        root, pointer_event(InputEventType::PointerMove, 30.0f, 20.0f, 0.1, 5, PointerKind::Touch));
    harness.feed(
        root, pointer_event(InputEventType::PointerMove, 30.0f, 0.0f, 0.2, 5, PointerKind::Touch));
    return expect(near(inner.scroll_y(), 20.0f), "the innermost container scrolls") &&
           expect(near(outer.scroll_y(), 0.0f), "the outer container is untouched");
}

bool deterministic_replay() {
    const auto run = [](float& offset, std::vector<GestureEvent>& log) {
        PanScene scene;
        Harness harness;
        harness.feed(scene.root, pointer_event(InputEventType::PointerDown, 25.0f, 25.0f, 0.0, 9,
                                               PointerKind::Touch));
        harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, 5.0f, 0.1, 9,
                                               PointerKind::Touch));
        harness.feed(scene.root, pointer_event(InputEventType::PointerMove, 25.0f, -25.0f, 0.2, 9,
                                               PointerKind::Touch));
        harness.feed(scene.root, pointer_event(InputEventType::PointerUp, 25.0f, -25.0f, 0.3, 9,
                                               PointerKind::Touch));
        offset = scene.container->scroll_y();
        log = harness.gestures;
    };

    float first_offset = 0.0f;
    float second_offset = 0.0f;
    std::vector<GestureEvent> first_log;
    std::vector<GestureEvent> second_log;
    run(first_offset, first_log);
    run(second_offset, second_log);

    if (!expect(near(first_offset, 30.0f), "script offset") ||
        !expect(near(first_offset, second_offset), "same script, same offset")) {
        return false;
    }
    if (!expect(first_log.size() == second_log.size(), "same gesture count")) {
        return false;
    }
    for (std::size_t index = 0; index < first_log.size(); ++index) {
        if (!expect(first_log[index].kind == second_log[index].kind, "gesture kind") ||
            !expect(first_log[index].phase == second_log[index].phase, "gesture phase") ||
            !expect(near(first_log[index].dx, second_log[index].dx), "gesture dx") ||
            !expect(near(first_log[index].dy, second_log[index].dy), "gesture dy")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!drag_scrolls_touch_and_mouse()) {
        return 1;
    }
    if (!clamps_on_every_move_and_keeps_claiming()) {
        return 2;
    }
    if (!cancel_stops_the_pan()) {
        return 3;
    }
    if (!non_scrollable_leaves_the_gesture_alone()) {
        return 4;
    }
    if (!innermost_container_consumes()) {
        return 5;
    }
    if (!deterministic_replay()) {
        return 6;
    }

    std::cout << "scroll_pan: ok\n";
    return 0;
}
