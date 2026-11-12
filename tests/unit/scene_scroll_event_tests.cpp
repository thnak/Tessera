// SPDX-License-Identifier: MIT

// Scene-side Scroll event (AGENTS.md V1-M2 item "Scene-side EventType::Scroll for virtual
// collections"). The scene emits exactly one Scroll to a scroll container whose offset actually
// changed, in capture -> target -> bubble order, with `scroll_x`/`scroll_y` carrying the applied
// delta and `ScrollEventInfo` the resulting offsets plus the container's viewport/content metrics.
// Clamped, non-scrollable, and no-op requests emit nothing; a handler that scrolls the same
// container again applies the move without re-emitting. The wheel path is driven through
// `InputRouter` with synthetic input, like the other scene tests: no wall clock, no real device.

#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/event_payloads.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 0.001f;
}

void check_near(float actual, float expected, const char* message) {
    if (!near(actual, expected)) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

struct SeenScroll {
    ui::NodeId node = 0;
    ui::EventPhase phase = ui::EventPhase::Target;
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    ui::ScrollEventInfo info;
    bool handled_before = false;
};

// Records every Scroll phase it sees and optionally behaves like a real handler: consume, prevent,
// or scroll again from inside the event.
class Recorder final : public ui::Node {
public:
    Recorder(ui::NodeId id, std::vector<SeenScroll>* log)
        : ui::Node(id),
          log_(log) {}

    void on_event(ui::Event& event) override {
        if (event.type != ui::EventType::Scroll) {
            return;
        }
        SeenScroll seen;
        seen.node = id();
        seen.phase = event.phase;
        seen.delta_x = event.scroll_x;
        seen.delta_y = event.scroll_y;
        seen.info = ui::scroll_event_info(event);
        seen.handled_before = event.handled;
        log_->push_back(seen);
        if (handle) {
            event.handled = true;
        }
        if (prevent) {
            event.prevent_default = true;
        }
        if (nested_target != nullptr && event.phase == ui::EventPhase::Target && !nested_done) {
            nested_done = true;
            ++nested_calls;
            ui::scroll_by(*nested_target, nested_dx, nested_dy);
        }
    }

    bool handle = false;
    bool prevent = false;
    ui::Node* nested_target = nullptr;
    float nested_dx = 0.0f;
    float nested_dy = 0.0f;
    bool nested_done = false;
    int nested_calls = 0;

private:
    std::vector<SeenScroll>* log_;
};

// Outer root 200x200; scroll container at {10,10,100,100} with a 5 px scrollport inset on every
// side (so the scrollport is 90x90) and 100x300 of content; one inner child inside the container.
struct ScrollableScene {
    ScrollableScene() {
        root.set_bounds({0, 0, 200, 200});
        auto container_owner = std::make_unique<Recorder>(2, &log);
        container = container_owner.get();
        container->set_bounds({10, 10, 100, 100});
        container->set_scrollport_insets(5.0f, 5.0f, 5.0f, 5.0f);
        container->set_scrollable_y(true);
        container->set_content_size(100.0f, 300.0f);
        root.add_child(std::move(container_owner));

        auto child_owner = std::make_unique<Recorder>(3, &log);
        child = child_owner.get();
        child->set_bounds({10, 10, 20, 20});
        container->add_child(std::move(child_owner));
    }

    std::vector<SeenScroll> log;
    Recorder root{1, &log};
    Recorder* container = nullptr;
    Recorder* child = nullptr;
};

ui::InputEvent wheel(float x, float y, float dx, float dy) {
    ui::InputEvent event;
    event.type = ui::InputEventType::Wheel;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    return event;
}

// 1. Wheel scrolling emits one Scroll, the applied delta/offsets match the movement, the payload
//    carries the node's real metrics, and the event reaches the container and its ancestor in
//    capture -> target -> bubble order.
void wheel_emits_one_scroll_in_order() {
    ScrollableScene scene;
    scene.container->handle = true;
    scene.container->prevent = true;

    ui::InputRouter router;
    const ui::InputDispatchResult result =
        router.dispatch(scene.root, wheel(35.0f, 35.0f, 0.0f, 1.0f));
    check(result.dispatched, "the wheel dispatches");
    check(result.target == scene.child, "the wheel targets the deepest hit node");
    check_near(scene.container->scroll_y(), ui::kPixelsPerWheelUnit,
               "one wheel unit scrolls one converted step");

    check(scene.log.size() == 3,
          "one Scroll event is recorded: capture, target and bubble each once");
    if (scene.log.size() == 3) {
        check(scene.log[0].node == 1 && scene.log[0].phase == ui::EventPhase::Capture,
              "the ancestor captures first");
        check(scene.log[1].node == 2 && scene.log[1].phase == ui::EventPhase::Target,
              "the container is the target");
        check(scene.log[2].node == 1 && scene.log[2].phase == ui::EventPhase::Bubble,
              "the ancestor bubbles last");

        const SeenScroll& target = scene.log[1];
        check_near(target.delta_x, 0.0f, "no horizontal delta was applied");
        check_near(target.delta_y, ui::kPixelsPerWheelUnit, "the delta is the applied movement");
        check_near(target.info.offset_x, 0.0f, "the payload carries the resulting x offset");
        check_near(target.info.offset_y, ui::kPixelsPerWheelUnit,
                   "the payload carries the resulting y offset");
        check_near(target.info.viewport_width, 90.0f,
                   "the viewport is the border box inset by the scrollport insets");
        check_near(target.info.viewport_height, 90.0f,
                   "the viewport height comes from the node, not a zero default");
        check_near(target.info.content_width, 100.0f, "the content width comes from the node");
        check_near(target.info.content_height, 300.0f, "the content height comes from the node");
        check(scene.log[2].handled_before,
              "handled is recorded on the event and does not stop propagation");
    }

    // prevent_default is advisory for a Scroll: the move already happened.
    check_near(scene.container->scroll_y(), ui::kPixelsPerWheelUnit,
               "prevent_default cannot undo the applied scroll");
}

// 2. Clamped, non-scrollable, and no-op requests emit nothing.
void no_event_without_movement() {
    ScrollableScene scene;
    ui::InputRouter router;

    // The move to the clamped bottom emits its own event; clear it before the assertions.
    check(ui::scroll_to(*scene.container, 0.0f, 1000.0f), "the move to the bottom applies");
    check_near(scene.container->scroll_y(), 210.0f,
               "the maximum offset honours the inset scrollport");
    scene.log.clear();

    const ui::InputDispatchResult clamped =
        router.dispatch(scene.root, wheel(35.0f, 35.0f, 0.0f, 1.0f));
    check(clamped.dispatched, "a wheel at the clamped edge still dispatches");
    check(scene.log.empty(), "a clamped edge emits no Scroll event");

    check(!ui::scroll_by(*scene.container, 0.0f, 0.0f), "a zero request reports no change");
    check(scene.log.empty(), "a zero-delta request emits no Scroll event");

    check(!ui::scroll_by(*scene.container, 10.0f, 0.0f), "a non-scrollable axis reports no change");
    check(scene.log.empty(), "a non-scrollable-axis request emits no Scroll event");

    check(
        !ui::scroll_to(*scene.container, scene.container->scroll_x(), scene.container->scroll_y()),
        "the same offset reports no change");
    check(scene.log.empty(), "an unchanged offset emits no Scroll event");

    const ui::InputDispatchResult plain =
        router.dispatch(scene.root, wheel(150.0f, 150.0f, 0.0f, 1.0f));
    check(plain.dispatched, "a wheel over the plain root still dispatches");
    check(scene.log.empty(), "a non-scrollable chain emits no Scroll event");
}

// 3. scroll_into_view emits one event per container it moves and nothing when the node is already
//    visible.
void scroll_into_view_emits_only_when_it_moves() {
    std::vector<SeenScroll> log;
    Recorder root(1, &log);
    root.set_bounds({0, 0, 100, 100});
    root.set_scrollable_x(true);
    root.set_scrollable_y(true);
    root.set_content_size(300.0f, 300.0f);
    auto child_owner = std::make_unique<ui::Node>(2);
    ui::Node& child = *child_owner;
    child.set_bounds({150, 150, 20, 20});
    root.add_child(std::move(child_owner));

    check(ui::scroll_into_view(child), "scroll_into_view moves the container");
    check_near(root.scroll_x(), 70.0f, "the minimal x offset is applied");
    check_near(root.scroll_y(), 70.0f, "the minimal y offset is applied");
    check(log.size() == 1, "one Scroll event for the moved container");
    if (log.size() == 1) {
        check(log[0].node == 1 && log[0].phase == ui::EventPhase::Target,
              "the moved container is the target");
        check_near(log[0].delta_x, 70.0f, "the x delta is the applied movement");
        check_near(log[0].delta_y, 70.0f, "the y delta is the applied movement");
        check_near(log[0].info.offset_x, 70.0f, "the payload offset x matches the node");
        check_near(log[0].info.offset_y, 70.0f, "the payload offset y matches the node");
        check_near(log[0].info.viewport_width, 100.0f, "the payload viewport width");
        check_near(log[0].info.viewport_height, 100.0f, "the payload viewport height");
        check_near(log[0].info.content_width, 300.0f, "the payload content width");
        check_near(log[0].info.content_height, 300.0f, "the payload content height");
    }

    check(!ui::scroll_into_view(child), "an already-visible node does not scroll");
    check(log.size() == 1, "an already-visible node emits no second Scroll event");
}

// 4. A handler that scrolls the same container during its own dispatch is bounded: the nested move
//    applies, but it does not synthesize a second event (the dispatcher itself has no such guard).
void self_scrolling_handler_is_bounded() {
    ScrollableScene scene;
    scene.container->nested_target = scene.container;
    scene.container->nested_dy = 10.0f;

    ui::InputRouter router;
    const ui::InputDispatchResult result =
        router.dispatch(scene.root, wheel(35.0f, 35.0f, 0.0f, 1.0f));
    check(result.dispatched, "the outer wheel dispatches");
    check(scene.container->nested_calls == 1, "the handler's nested scroll ran exactly once");
    std::size_t target_events = 0;
    for (const SeenScroll& seen : scene.log) {
        if (seen.node == 2 && seen.phase == ui::EventPhase::Target) {
            ++target_events;
        }
    }
    check(target_events == 1, "a nested scroll on the same container does not re-emit");
    check(scene.log.size() == 3,
          "only the outer event's capture/target/bubble phases are recorded");
    check_near(scene.container->scroll_y(), ui::kPixelsPerWheelUnit + 10.0f,
               "the nested move still applies after the guard suppresses its event");
}

} // namespace

int main() {
    wheel_emits_one_scroll_in_order();
    no_event_without_movement();
    scroll_into_view_emits_only_when_it_moves();
    self_scrolling_handler_is_bounded();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "scene scroll event tests passed\n";
    return 0;
}
