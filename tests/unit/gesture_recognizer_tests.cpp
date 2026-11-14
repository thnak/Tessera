// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture.hpp"
#include "tessera/ui/gesture_recognizers.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

using tessera::ui::DragRecognizer;
using tessera::ui::GestureArena;
using tessera::ui::GestureConfig;
using tessera::ui::GestureEvent;
using tessera::ui::GestureKind;
using tessera::ui::GesturePhase;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::LongPressRecognizer;
using tessera::ui::PinchRotateRecognizer;
using tessera::ui::SwipeRecognizer;
using tessera::ui::TapRecognizer;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gesture_recognizers: " << message << "\n";
    }
    return condition;
}

InputEvent make_event(InputEventType type, std::uint32_t pointer_id, float x, float y,
                      double timestamp_s) {
    InputEvent event;
    event.type = type;
    event.pointer_id = pointer_id;
    event.x = x;
    event.y = y;
    event.timestamp_s = timestamp_s;
    return event;
}

InputEvent down(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return make_event(InputEventType::PointerDown, pointer_id, x, y, timestamp_s);
}

InputEvent move(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return make_event(InputEventType::PointerMove, pointer_id, x, y, timestamp_s);
}

InputEvent up(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return make_event(InputEventType::PointerUp, pointer_id, x, y, timestamp_s);
}

bool single_event(const GestureArena& arena, GestureKind kind, GesturePhase phase) {
    const auto events = arena.events();
    return events.size() == 1 && events[0].kind == kind && events[0].phase == phase;
}

bool tap_semantics() {
    TapRecognizer tap;
    GestureArena arena;
    arena.add(tap);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    if (!expect(arena.events().empty(), "tap emits nothing on down")) {
        return false;
    }
    arena.process(move(1, 5.0f, 0.0f, 0.01));
    if (!expect(arena.events().empty(), "tap within slop emits nothing on move")) {
        return false;
    }
    arena.process(up(1, 6.0f, 0.0f, 0.02));
    const auto tap_events = arena.events();
    if (!expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended), "tap ended event") ||
        !expect(tap_events[0].pointer_id == 1, "tap pointer id") ||
        !expect(tap_events[0].x == 6.0f && tap_events[0].y == 0.0f, "tap position")) {
        return false;
    }

    arena.process(down(1, 0.0f, 0.0f, 0.1));
    arena.process(up(1, 0.0f, 0.0f, 0.15));
    if (!expect(single_event(arena, GestureKind::DoubleTap, GesturePhase::Ended),
                "second tap inside the window is a double tap")) {
        return false;
    }

    arena.process(down(1, 0.0f, 0.0f, 1.0));
    arena.process(up(1, 0.0f, 0.0f, 1.1));
    return expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended),
                  "tap after a double tap starts a new sequence");
}

bool tap_window_rejects() {
    GestureConfig config; // double_tap_ms = 300
    TapRecognizer tap;
    GestureArena arena(config);
    arena.add(tap);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(up(1, 0.0f, 0.0f, 0.05));
    arena.process(down(1, 0.0f, 0.0f, 0.5));
    arena.process(up(1, 0.0f, 0.0f, 0.55));
    if (!expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended),
                "tap outside the double-tap window stays a tap")) {
        return false;
    }

    // Too far apart spatially is also a new tap.
    arena.process(down(1, 0.0f, 0.0f, 0.6));
    arena.process(up(1, 0.0f, 0.0f, 0.65));
    arena.process(down(1, 50.0f, 0.0f, 0.7));
    arena.process(up(1, 50.0f, 0.0f, 0.75));
    return expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended),
                  "tap outside the spatial slop stays a tap");
}

bool slop_boundary_and_drag() {
    GestureConfig config;
    config.tap_slop = 8.0f;
    config.drag_threshold = 10.0f;
    TapRecognizer tap;
    DragRecognizer drag;
    GestureArena arena(config);
    arena.add(tap);
    arena.add(drag);

    // Exactly at the tap slop is still a tap.
    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(move(1, 8.0f, 0.0f, 0.01));
    arena.process(up(1, 8.0f, 0.0f, 0.02));
    if (!expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended),
                "tap at the slop boundary")) {
        return false;
    }

    // Past the tap slop but below the drag threshold: no gesture at all.
    arena.process(down(1, 0.0f, 0.0f, 0.1));
    arena.process(move(1, 9.0f, 0.0f, 0.11));
    arena.process(up(1, 9.0f, 0.0f, 0.12));
    if (!expect(arena.events().empty(), "no gesture between tap slop and drag threshold")) {
        return false;
    }

    // Past the drag threshold the drag begins and the tap is gone.
    arena.process(down(1, 0.0f, 0.0f, 0.2));
    arena.process(move(1, 11.0f, 0.0f, 0.21));
    const auto began = arena.events();
    if (!expect(began.size() == 1 && began[0].kind == GestureKind::Drag &&
                    began[0].phase == GesturePhase::Began,
                "drag begins past the threshold") ||
        !expect(began[0].dx == 11.0f, "drag begin dx is the total displacement")) {
        return false;
    }
    arena.process(up(1, 11.0f, 0.0f, 0.22));
    return expect(single_event(arena, GestureKind::Drag, GesturePhase::Ended),
                  "drag ends with a drag, never a tap");
}

bool drag_began_once() {
    DragRecognizer drag;
    GestureArena arena;
    arena.add(drag);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(move(1, 12.0f, 0.0f, 0.01));
    if (!expect(single_event(arena, GestureKind::Drag, GesturePhase::Began), "drag began")) {
        return false;
    }
    arena.process(move(1, 20.0f, 0.0f, 0.02));
    const auto changed = arena.events();
    if (!expect(changed.size() == 1 && changed[0].phase == GesturePhase::Changed, "drag changed") ||
        !expect(changed[0].dx == 20.0f, "drag changed total dx")) {
        return false;
    }
    arena.process(move(1, 30.0f, 0.0f, 0.03));
    arena.process(up(1, 30.0f, 0.0f, 0.04));
    const auto ended = arena.events();
    return expect(ended.size() == 1 && ended[0].phase == GesturePhase::Ended, "one drag ended") &&
           expect(ended[0].dx == 30.0f, "drag ended total dx");
}

bool long_press_semantics() {
    LongPressRecognizer long_press;
    TapRecognizer tap;
    GestureArena arena;
    arena.add(long_press);
    arena.add(tap);

    // Held past the threshold: began on tick, ended on release.
    arena.process(down(1, 5.0f, 5.0f, 0.0));
    arena.tick(0.4);
    if (!expect(arena.events().empty(), "long press before the threshold is silent")) {
        return false;
    }
    arena.tick(0.6);
    if (!expect(single_event(arena, GestureKind::LongPress, GesturePhase::Began),
                "long press began on tick")) {
        return false;
    }
    arena.process(up(1, 5.0f, 5.0f, 0.7));
    if (!expect(single_event(arena, GestureKind::LongPress, GesturePhase::Ended),
                "long press ended on release")) {
        return false;
    }

    // Early release falls through to the tap recognizer.
    arena.process(down(1, 5.0f, 5.0f, 1.0));
    arena.tick(1.2);
    arena.process(up(1, 5.0f, 5.0f, 1.2));
    if (!expect(single_event(arena, GestureKind::Tap, GesturePhase::Ended),
                "early release is a tap")) {
        return false;
    }

    // Moving past the slop cancels the long press and leaves no gesture.
    arena.process(down(1, 5.0f, 5.0f, 2.0));
    arena.process(move(1, 25.0f, 5.0f, 2.1));
    arena.tick(2.7);
    arena.process(up(1, 25.0f, 5.0f, 2.8));
    return expect(arena.events().empty(), "long press rejects past the slop");
}

bool swipe_semantics() {
    SwipeRecognizer swipe;
    GestureArena arena;
    arena.add(swipe);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(move(1, 100.0f, 0.0f, 0.05));
    const auto began = arena.events();
    if (!expect(began.size() == 1 && began[0].kind == GestureKind::Swipe &&
                    began[0].phase == GesturePhase::Began,
                "fast horizontal move begins a swipe") ||
        !expect(began[0].dx > 0.0f, "swipe direction is positive x")) {
        return false;
    }
    arena.process(move(1, 150.0f, 0.0f, 0.1));
    arena.process(up(1, 160.0f, 0.0f, 0.15));
    const auto ended = arena.events();
    if (!expect(ended.size() == 1 && ended[0].phase == GesturePhase::Ended, "swipe ended") ||
        !expect(ended[0].dx == 160.0f && ended[0].dy == 0.0f, "swipe ended displacement")) {
        return false;
    }

    // Slow movement stays below the velocity threshold.
    arena.process(down(1, 0.0f, 0.0f, 1.0));
    arena.process(move(1, 20.0f, 0.0f, 3.0));
    arena.process(up(1, 20.0f, 0.0f, 3.1));
    if (!expect(arena.events().empty(), "slow drag is not a swipe")) {
        return false;
    }

    // Vertical direction is the sign of dy.
    arena.process(down(1, 0.0f, 0.0f, 4.0));
    arena.process(move(1, 0.0f, 100.0f, 4.05));
    const auto vertical = arena.events();
    return expect(vertical.size() == 1 && vertical[0].phase == GesturePhase::Began,
                  "vertical swipe begins") &&
           expect(vertical[0].dy > 0.0f, "vertical swipe direction is positive y");
}

bool pinch_rotate_semantics() {
    GestureConfig config;
    config.simultaneous = true;
    PinchRotateRecognizer pinch;
    GestureArena arena(config);
    arena.add(pinch);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(down(2, 100.0f, 0.0f, 0.01));
    if (!expect(arena.events().empty(), "two pointers down emit nothing yet")) {
        return false;
    }

    arena.process(move(1, -50.0f, 0.0f, 0.05));
    const auto begun = arena.events();
    if (!expect(begun.size() == 2, "pinch began emits pinch and rotate") ||
        !expect(begun[0].kind == GestureKind::Pinch && begun[1].kind == GestureKind::Rotate,
                "pinch/rotate kinds") ||
        !expect(begun[0].pointer_id == 1 && begun[0].secondary_pointer_id == 2,
                "two pointer ids carried") ||
        !expect(std::fabs(begun[0].scale - 1.5f) < 0.001f, "pinch scale is distance ratio")) {
        return false;
    }
    if (!expect(arena.claimed(1) && arena.claimed(2), "pinch claims both pointers")) {
        return false;
    }

    arena.process(move(2, 0.0f, 100.0f, 0.1));
    const auto changed = arena.events();
    if (!expect(changed.size() == 2 && changed[0].phase == GesturePhase::Changed &&
                    changed[1].phase == GesturePhase::Changed,
                "pinch/rotate changed") ||
        !expect(changed[1].rotation > 0.0f, "rotation follows the second pointer")) {
        return false;
    }

    arena.process(up(1, -50.0f, 0.0f, 0.15));
    const auto ended = arena.events();
    return expect(ended.size() == 2 && ended[0].phase == GesturePhase::Ended &&
                      ended[1].phase == GesturePhase::Ended,
                  "pinch/rotate ended on first release") &&
           expect(!arena.tracking(1) && !arena.tracking(2), "both pointers released");
}

bool pinch_requires_simultaneous() {
    GestureConfig config;
    config.simultaneous = false;
    PinchRotateRecognizer pinch;
    GestureArena arena(config);
    arena.add(pinch);

    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(down(2, 100.0f, 0.0f, 0.01));
    if (!expect(!arena.tracking(2), "second pointer ignored without simultaneous")) {
        return false;
    }
    arena.process(move(1, -50.0f, 0.0f, 0.02));
    return expect(!arena.claimed(1) && arena.events().empty(),
                  "pinch never claims a single pointer");
}

bool cancel_mid_gesture() {
    DragRecognizer drag;
    GestureArena arena;
    arena.add(drag);
    arena.process(down(1, 0.0f, 0.0f, 0.0));
    arena.process(move(1, 20.0f, 0.0f, 0.01));
    if (!expect(single_event(arena, GestureKind::Drag, GesturePhase::Began), "drag began")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerCancel, 1, 20.0f, 0.0f, 0.02));
    const auto canceled = arena.events();
    if (!expect(canceled.size() == 1 && canceled[0].phase == GesturePhase::Canceled,
                "cancel emits Canceled") ||
        !expect(canceled[0].kind == GestureKind::Drag, "cancel keeps the gesture kind")) {
        return false;
    }
    arena.process(up(1, 20.0f, 0.0f, 0.03));
    if (!expect(arena.events().empty(), "up after cancel emits nothing")) {
        return false;
    }

    TapRecognizer tap;
    GestureArena tap_arena;
    tap_arena.add(tap);
    tap_arena.process(down(2, 0.0f, 0.0f, 0.0));
    tap_arena.process(make_event(InputEventType::PointerCancel, 2, 0.0f, 0.0f, 0.01));
    return expect(tap_arena.events().empty(), "canceled tap emits nothing");
}

} // namespace

int main() {
    if (!tap_semantics()) {
        return 1;
    }
    if (!tap_window_rejects()) {
        return 2;
    }
    if (!slop_boundary_and_drag()) {
        return 3;
    }
    if (!drag_began_once()) {
        return 4;
    }
    if (!long_press_semantics()) {
        return 5;
    }
    if (!swipe_semantics()) {
        return 6;
    }
    if (!pinch_rotate_semantics()) {
        return 7;
    }
    if (!pinch_requires_simultaneous()) {
        return 8;
    }
    if (!cancel_mid_gesture()) {
        return 9;
    }

    std::cout << "gesture_recognizers: ok\n";
    return 0;
}
