// SPDX-License-Identifier: MIT

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/gesture.hpp"
#include "tessera/ui/gesture_recognizers.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::DragRecognizer;
using tessera::ui::Event;
using tessera::ui::GestureArena;
using tessera::ui::GestureEvent;
using tessera::ui::GestureKind;
using tessera::ui::GesturePhase;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputPump;
using tessera::ui::LongPressRecognizer;
using tessera::ui::Node;
using tessera::ui::PinchRotateRecognizer;
using tessera::ui::PointerKind;
using tessera::ui::SwipeRecognizer;
using tessera::ui::TapRecognizer;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gesture_pipeline: " << message << "\n";
    }
    return condition;
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        log.push_back(std::to_string(id()) + ":" + std::to_string(static_cast<int>(event.type)) +
                      ":" + std::to_string(static_cast<int>(event.phase)));
    }

    std::vector<std::string> log;
};

struct Scene {
    Scene() {
        root.set_bounds({0, 0, 300, 200});
        auto child_ptr = std::make_unique<Recorder>(2);
        child = child_ptr.get();
        child->set_bounds({10, 10, 80, 80});
        root.add_child(std::move(child_ptr));

        auto other_ptr = std::make_unique<Recorder>(3);
        other = other_ptr.get();
        other->set_bounds({150, 120, 80, 60});
        root.add_child(std::move(other_ptr));
    }

    Recorder root{1};
    Recorder* child = nullptr;
    Recorder* other = nullptr;
};

InputEvent pointer_event(InputEventType type, float x, float y, double timestamp_s,
                         std::uint32_t pointer_id = 1) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.timestamp_s = timestamp_s;
    event.pointer_id = pointer_id;
    // Touch keeps the router's hover state out of the picture; the arena ignores the kind.
    event.pointer_kind = PointerKind::Touch;
    return event;
}

void collect_gesture(void* user, const GestureEvent& event) {
    static_cast<std::vector<GestureEvent>*>(user)->push_back(event);
}

InputEvent down(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return pointer_event(InputEventType::PointerDown, x, y, timestamp_s, pointer_id);
}

InputEvent move(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return pointer_event(InputEventType::PointerMove, x, y, timestamp_s, pointer_id);
}

InputEvent up(std::uint32_t pointer_id, float x, float y, double timestamp_s) {
    return pointer_event(InputEventType::PointerUp, x, y, timestamp_s, pointer_id);
}

void feed(InputPump& pump, RuntimeLoop& loop, Node& root, const InputEvent& event) {
    pump.queue().push(event);
    pump.pump(root, loop);
}

bool approx(float value, float expected) {
    return std::fabs(value - expected) < 0.01f;
}

bool tap_and_double_tap() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    feed(pump, loop, scene.root, up(1, 20.0f, 20.0f, 0.05));
    if (!expect(gestures.size() == 1, "tap produces one gesture") ||
        !expect(gestures[0].kind == GestureKind::Tap, "tap kind") ||
        !expect(gestures[0].phase == GesturePhase::Ended, "tap phase") ||
        !expect(gestures[0].target_id == scene.child->id(), "tap target id is the hit node") ||
        !expect(gestures[0].pointer_id == 1, "tap pointer id")) {
        return false;
    }

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.1));
    feed(pump, loop, scene.root, up(1, 20.0f, 20.0f, 0.15));
    return expect(gestures.size() == 2, "second tap produces one more gesture") &&
           expect(gestures[1].kind == GestureKind::DoubleTap, "second tap is a double tap") &&
           expect(gestures[1].target_id == scene.child->id(), "double tap target id");
}

bool long_press_uses_sampled_time() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    clock.advance(0.6);
    pump.tick(clock.now_seconds());
    if (!expect(gestures.size() == 1, "long press began on tick") ||
        !expect(gestures[0].kind == GestureKind::LongPress, "long press kind") ||
        !expect(gestures[0].phase == GesturePhase::Began, "long press began phase") ||
        !expect(approx(static_cast<float>(gestures[0].timestamp_s), 0.6f),
                "tick timestamp is the sampled time") ||
        !expect(gestures[0].target_id == scene.child->id(), "long press target id")) {
        return false;
    }
    feed(pump, loop, scene.root, up(1, 20.0f, 20.0f, 0.7));
    return expect(gestures.size() == 2 && gestures[1].phase == GesturePhase::Ended,
                  "long press ended on release");
}

bool drag_and_swipe() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    // Slow movement: below the swipe velocity window, so the drag recognizer wins.
    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    feed(pump, loop, scene.root, move(1, 30.0f, 20.0f, 0.05));
    feed(pump, loop, scene.root, move(1, 60.0f, 20.0f, 0.10));
    feed(pump, loop, scene.root, up(1, 60.0f, 20.0f, 0.15));
    if (!expect(gestures.size() == 3, "drag emits began/changed/ended") ||
        !expect(gestures[0].kind == GestureKind::Drag && gestures[0].phase == GesturePhase::Began,
                "drag began") ||
        !expect(gestures[1].phase == GesturePhase::Changed, "drag changed") ||
        !expect(gestures[2].phase == GesturePhase::Ended, "drag ended") ||
        !expect(approx(gestures[2].dx, 40.0f), "drag ended dx is the total displacement") ||
        !expect(gestures[0].target_id == scene.child->id(), "drag target id")) {
        return false;
    }

    gestures.clear();
    // Fast movement claims the swipe recognizer instead.
    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 1.0));
    feed(pump, loop, scene.root, move(1, 120.0f, 20.0f, 1.05));
    feed(pump, loop, scene.root, up(1, 120.0f, 20.0f, 1.10));
    return expect(gestures.size() == 2, "swipe emits began/ended") &&
           expect(gestures[0].kind == GestureKind::Swipe &&
                      gestures[0].phase == GesturePhase::Began,
                  "swipe began") &&
           expect(gestures[1].phase == GesturePhase::Ended, "swipe ended") &&
           expect(gestures[1].dx > 0.0f, "swipe direction") &&
           expect(gestures[0].target_id == scene.child->id(), "swipe target id");
}

bool two_pointer_targets_first_slot() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    feed(pump, loop, scene.root, down(2, 200.0f, 160.0f, 0.01));
    feed(pump, loop, scene.root, move(1, 40.0f, 20.0f, 0.05));
    if (!expect(gestures.size() == 2, "pinch emits both kinds on claim") ||
        !expect(gestures[0].kind == GestureKind::Pinch, "pinch first") ||
        !expect(gestures[1].kind == GestureKind::Rotate, "rotate second") ||
        !expect(gestures[0].pointer_id == 1 && gestures[0].secondary_pointer_id == 2,
                "pinch carries both pointer ids") ||
        !expect(gestures[0].target_id == scene.child->id(),
                "two-pointer target is the first pointer's down target")) {
        return false;
    }
    feed(pump, loop, scene.root, up(1, 40.0f, 20.0f, 0.10));
    return expect(gestures.size() == 4 && gestures[2].phase == GesturePhase::Ended,
                  "pinch ends when the first pointer lifts") &&
           expect(gestures[2].target_id == scene.child->id(), "pinch ended keeps the target id");
}

bool target_id_stable_after_removal() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    feed(pump, loop, scene.root, move(1, 40.0f, 20.0f, 0.05));
    const tessera::ui::NodeId target_id = scene.child->id();
    std::unique_ptr<Node> removed = scene.root.remove_child(*scene.child);
    scene.child = nullptr;
    removed.reset(); // destroy the hit node before the gesture finishes
    feed(pump, loop, scene.root, move(1, 70.0f, 20.0f, 0.10));
    feed(pump, loop, scene.root, up(1, 70.0f, 20.0f, 0.15));
    return expect(gestures.size() == 3, "drag survives the target being removed") &&
           expect(gestures[2].phase == GesturePhase::Ended, "drag still ends") &&
           expect(gestures[2].target_id == target_id,
                  "removed target is reported as the stored id, never dereferenced");
}

bool cancel_and_blur() {
    Scene scene;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);

    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 0.0));
    feed(pump, loop, scene.root, move(1, 40.0f, 20.0f, 0.05));
    InputEvent cancel = pointer_event(InputEventType::PointerCancel, 40.0f, 20.0f, 0.06);
    feed(pump, loop, scene.root, cancel);
    if (!expect(gestures.size() == 2, "cancel emits a canceled gesture") ||
        !expect(gestures[1].phase == GesturePhase::Canceled, "cancel phase") ||
        !expect(gestures[1].kind == GestureKind::Drag, "cancel keeps the gesture kind") ||
        !expect(gestures[1].target_id == scene.child->id(), "cancel keeps the target id")) {
        return false;
    }
    feed(pump, loop, scene.root, up(1, 40.0f, 20.0f, 0.07));
    if (!expect(gestures.size() == 2, "up after cancel emits nothing")) {
        return false;
    }

    gestures.clear();
    feed(pump, loop, scene.root, down(1, 20.0f, 20.0f, 1.0));
    feed(pump, loop, scene.root, move(1, 40.0f, 20.0f, 1.05));
    InputEvent blur;
    blur.type = InputEventType::Blur;
    blur.timestamp_s = 1.06;
    feed(pump, loop, scene.root, blur);
    return expect(gestures.size() == 2 && gestures[1].phase == GesturePhase::Canceled,
                  "blur cancels the in-flight gesture");
}

bool pointer_route_log_unchanged() {
    const std::vector<InputEvent> script{
        down(1, 20.0f, 20.0f, 0.0),  move(1, 30.0f, 25.0f, 0.01), move(1, 200.0f, 160.0f, 0.02),
        up(1, 200.0f, 160.0f, 0.03), move(1, 20.0f, 20.0f, 0.04),
    };

    Scene with_gestures;
    Scene without_gestures;
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump gestures_pump;
    gestures_pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    gestures_pump.set_gesture_callback(&collect_gesture, &gestures);
    InputPump plain_pump;

    for (const InputEvent& event : script) {
        feed(gestures_pump, loop, with_gestures.root, event);
        feed(plain_pump, loop, without_gestures.root, event);
    }

    return expect(!gestures.empty(), "gesture pipeline produced events") &&
           expect(with_gestures.root.log == without_gestures.root.log,
                  "root route log is identical with a gesture sink installed") &&
           expect(with_gestures.child->log == without_gestures.child->log,
                  "child route log is identical with a gesture sink installed") &&
           expect(with_gestures.other->log == without_gestures.other->log,
                  "sibling route log is identical with a gesture sink installed");
}

std::string describe(const GestureEvent& event) {
    return std::to_string(static_cast<int>(event.kind)) + ":" +
           std::to_string(static_cast<int>(event.phase)) + ":" + std::to_string(event.pointer_id) +
           ":" + std::to_string(event.secondary_pointer_id) + ":" +
           std::to_string(event.target_id) + ":" +
           std::to_string(static_cast<int>(std::lround(event.x * 100.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.y * 100.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.dx * 100.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.dy * 100.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.scale * 1000.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.rotation * 1000.0f))) + ":" +
           std::to_string(static_cast<int>(std::lround(event.timestamp_s * 1000.0)));
}

std::vector<std::string> serialize(const std::vector<GestureEvent>& events) {
    std::vector<std::string> log;
    log.reserve(events.size());
    for (const GestureEvent& event : events) {
        log.push_back(describe(event));
    }
    return log;
}

enum class StepKind : std::uint8_t { Down, Move, Up, Cancel, Blur, Tick };

struct Step {
    StepKind kind = StepKind::Down;
    float x = 0.0f;
    float y = 0.0f;
    double timestamp_s = 0.0;
};

// Same script for both paths. All downs land on the same hit node so the direct arena can be fed
// the identical target id that the pipeline resolves.
const std::vector<Step>& script() {
    static const std::vector<Step> steps{
        {StepKind::Down, 20.0f, 20.0f, 0.0},    {StepKind::Up, 20.0f, 20.0f, 0.05},
        {StepKind::Down, 20.0f, 20.0f, 0.1},    {StepKind::Up, 20.0f, 20.0f, 0.15},
        {StepKind::Down, 20.0f, 20.0f, 1.0},    {StepKind::Tick, 0.0f, 0.0f, 1.6},
        {StepKind::Up, 20.0f, 20.0f, 1.7},      {StepKind::Down, 20.0f, 20.0f, 2.0},
        {StepKind::Move, 30.0f, 20.0f, 2.05},   {StepKind::Move, 60.0f, 20.0f, 2.10},
        {StepKind::Up, 60.0f, 20.0f, 2.15},     {StepKind::Down, 20.0f, 20.0f, 3.0},
        {StepKind::Move, 120.0f, 20.0f, 3.05},  {StepKind::Up, 120.0f, 20.0f, 3.10},
        {StepKind::Down, 20.0f, 20.0f, 4.0},    {StepKind::Move, 40.0f, 20.0f, 4.05},
        {StepKind::Cancel, 40.0f, 20.0f, 4.10}, {StepKind::Down, 20.0f, 20.0f, 5.0},
        {StepKind::Move, 40.0f, 20.0f, 5.05},   {StepKind::Blur, 0.0f, 0.0f, 5.10},
    };
    return steps;
}

InputEvent script_event(const Step& step) {
    switch (step.kind) {
    case StepKind::Down:
        return down(1, step.x, step.y, step.timestamp_s);
    case StepKind::Move:
        return move(1, step.x, step.y, step.timestamp_s);
    case StepKind::Up:
        return up(1, step.x, step.y, step.timestamp_s);
    case StepKind::Cancel:
        return pointer_event(InputEventType::PointerCancel, step.x, step.y, step.timestamp_s);
    case StepKind::Blur: {
        InputEvent event;
        event.type = InputEventType::Blur;
        event.timestamp_s = step.timestamp_s;
        return event;
    }
    case StepKind::Tick:
        break;
    }
    return down(1, step.x, step.y, step.timestamp_s);
}

std::vector<std::string> run_pipeline(Scene& scene) {
    ManualClock clock;
    RuntimeLoop loop(clock);
    InputPump pump;
    pump.enable_gestures();
    std::vector<GestureEvent> gestures;
    pump.set_gesture_callback(&collect_gesture, &gestures);
    for (const Step& step : script()) {
        if (step.kind == StepKind::Tick) {
            pump.tick(step.timestamp_s);
            continue;
        }
        feed(pump, loop, scene.root, script_event(step));
    }
    return serialize(gestures);
}

bool deterministic_and_agrees_with_direct_arena() {
    Scene first_scene;
    Scene second_scene;
    const std::vector<std::string> pipeline_first = run_pipeline(first_scene);
    const std::vector<std::string> pipeline_second = run_pipeline(second_scene);
    if (!expect(!pipeline_first.empty(), "script produces a gesture log") ||
        !expect(pipeline_first == pipeline_second, "pipeline replay is identical across runs")) {
        return false;
    }

    // Feed the same script to a bare arena with the pipeline's recognizer order and target ids.
    Scene direct_scene;
    LongPressRecognizer long_press;
    TapRecognizer tap;
    PinchRotateRecognizer pinch;
    SwipeRecognizer swipe;
    DragRecognizer drag;
    GestureArena arena;
    arena.add(long_press);
    arena.add(tap);
    arena.add(pinch);
    arena.add(swipe);
    arena.add(drag);
    std::vector<std::string> direct_log;
    const auto capture = [&arena, &direct_log]() {
        for (const GestureEvent& event : arena.events()) {
            direct_log.push_back(describe(event));
        }
    };
    for (const Step& step : script()) {
        if (step.kind == StepKind::Tick) {
            arena.tick(step.timestamp_s);
            capture();
            continue;
        }
        const InputEvent event = script_event(step);
        arena.process(event, step.kind == StepKind::Down ? direct_scene.child->id() : 0);
        capture();
    }
    if (!expect(direct_log.size() == pipeline_first.size(),
                "pipeline and direct arena emit the same number of events")) {
        return false;
    }
    for (std::size_t i = 0; i < direct_log.size(); ++i) {
        if (!expect(direct_log[i] == pipeline_first[i],
                    "pipeline and direct arena emit the same event log")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!tap_and_double_tap()) {
        return 1;
    }
    if (!long_press_uses_sampled_time()) {
        return 2;
    }
    if (!drag_and_swipe()) {
        return 3;
    }
    if (!two_pointer_targets_first_slot()) {
        return 4;
    }
    if (!target_id_stable_after_removal()) {
        return 5;
    }
    if (!cancel_and_blur()) {
        return 6;
    }
    if (!pointer_route_log_unchanged()) {
        return 7;
    }
    if (!deterministic_and_agrees_with_direct_arena()) {
        return 8;
    }

    std::cout << "gesture_pipeline: ok\n";
    return 0;
}
