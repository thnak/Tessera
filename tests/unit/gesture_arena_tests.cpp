// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture.hpp"
#include "tessera/ui/gesture_recognizers.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::ui::GestureArena;
using tessera::ui::GestureConfig;
using tessera::ui::GestureContext;
using tessera::ui::GestureEvent;
using tessera::ui::GestureKind;
using tessera::ui::GesturePhase;
using tessera::ui::GestureRecognizer;
using tessera::ui::GestureSink;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::RecognizerAction;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gesture_arena: " << message << "\n";
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

// Probe recognizer: scripted decisions plus callback counters for the arena protocol tests.
class ProbeRecognizer : public GestureRecognizer {
public:
    explicit ProbeRecognizer(GestureKind kind)
        : kind_(kind) {}

    [[nodiscard]] GestureKind kind() const override {
        return kind_;
    }
    [[nodiscard]] bool multi_pointer() const override {
        return multi_pointer_;
    }

    RecognizerAction on_down(const InputEvent&, GestureContext) override {
        ++down_calls;
        return down_action;
    }
    RecognizerAction on_move(const InputEvent&, GestureContext) override {
        ++move_calls;
        return move_action;
    }
    RecognizerAction on_up(const InputEvent&, GestureContext) override {
        ++up_calls;
        return up_action;
    }
    RecognizerAction on_tick(double, GestureContext) override {
        ++tick_calls;
        return tick_action;
    }
    void on_cancel(const InputEvent&, GestureContext context) override {
        ++cancel_calls;
        if (!emit_cancel) {
            return;
        }
        GestureEvent event;
        event.kind = kind_;
        event.phase = GesturePhase::Canceled;
        context.sink.emit(event);
    }

    GestureKind kind_ = GestureKind::Tap;
    bool multi_pointer_ = false;
    RecognizerAction down_action = RecognizerAction::Ignore;
    RecognizerAction move_action = RecognizerAction::Ignore;
    RecognizerAction up_action = RecognizerAction::Ignore;
    RecognizerAction tick_action = RecognizerAction::Ignore;
    bool emit_cancel = true;
    int down_calls = 0;
    int move_calls = 0;
    int up_calls = 0;
    int tick_calls = 0;
    int cancel_calls = 0;
};

// Emits a configurable burst to exercise the bounded sink.
class BurstRecognizer : public GestureRecognizer {
public:
    [[nodiscard]] GestureKind kind() const override {
        return GestureKind::Tap;
    }
    RecognizerAction on_down(const InputEvent&, GestureContext context) override {
        for (int i = 0; i < 64; ++i) {
            GestureEvent event;
            event.kind = GestureKind::Tap;
            event.phase = GesturePhase::Ended;
            context.sink.emit(event);
        }
        return RecognizerAction::Claim;
    }
    RecognizerAction on_move(const InputEvent&, GestureContext) override {
        return RecognizerAction::Ignore;
    }
    RecognizerAction on_up(const InputEvent&, GestureContext) override {
        return RecognizerAction::Ignore;
    }
    RecognizerAction on_tick(double, GestureContext) override {
        return RecognizerAction::Ignore;
    }
    void on_cancel(const InputEvent&, GestureContext) override {}
};

bool losers_are_not_polled() {
    ProbeRecognizer reject_down(GestureKind::Tap);
    reject_down.down_action = RecognizerAction::Reject;
    ProbeRecognizer claimer(GestureKind::Drag);
    claimer.move_action = RecognizerAction::Claim;

    GestureArena arena;
    if (!expect(arena.add(reject_down), "add first") || !expect(arena.add(claimer), "add second")) {
        return false;
    }

    arena.process(make_event(InputEventType::PointerDown, 1, 10, 10, 0.0));
    if (!expect(reject_down.down_calls == 1 && claimer.down_calls == 1, "down polls in order") ||
        !expect(!arena.claimed(1), "no claim on down")) {
        return false;
    }

    arena.process(make_event(InputEventType::PointerMove, 1, 30, 10, 0.01));
    if (!expect(arena.claimed(1), "move claims") ||
        !expect(claimer.move_calls == 1, "claimer polled once") ||
        !expect(reject_down.move_calls == 0, "rejected recognizer is not polled again")) {
        return false;
    }

    arena.process(make_event(InputEventType::PointerMove, 1, 40, 10, 0.02));
    arena.process(make_event(InputEventType::PointerUp, 1, 40, 10, 0.03));
    return expect(claimer.move_calls == 2, "only the winner receives moves") &&
           expect(claimer.up_calls == 1, "only the winner receives up") &&
           expect(!arena.tracking(1), "up releases the pointer slot");
}

bool first_claim_wins() {
    ProbeRecognizer first(GestureKind::Tap);
    first.down_action = RecognizerAction::Claim;
    ProbeRecognizer second(GestureKind::Drag);
    second.down_action = RecognizerAction::Claim;

    GestureArena arena;
    arena.add(first);
    arena.add(second);
    arena.process(make_event(InputEventType::PointerDown, 2, 0, 0, 0.0));
    if (!expect(arena.claimed(2), "first claim on down wins") ||
        !expect(first.down_calls == 1, "first polled") ||
        !expect(second.down_calls == 0, "later recognizers are not polled after a claim")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerUp, 2, 0, 0, 0.01));
    return expect(first.up_calls == 1 && second.up_calls == 0, "winner owns move/up");
}

bool tick_claims() {
    ProbeRecognizer tapper(GestureKind::LongPress);
    tapper.tick_action = RecognizerAction::Claim;
    GestureArena arena;
    arena.add(tapper);
    arena.process(make_event(InputEventType::PointerDown, 3, 5, 5, 0.0));
    arena.tick(0.5);
    if (!expect(arena.claimed(3), "tick claims") ||
        !expect(tapper.tick_calls == 1, "tick polled once")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerMove, 3, 6, 5, 0.6));
    return expect(tapper.move_calls == 1, "winner receives moves after a tick claim");
}

bool cancel_emits_canceled() {
    ProbeRecognizer drag(GestureKind::Drag);
    drag.move_action = RecognizerAction::Claim;
    GestureArena arena;
    arena.add(drag);
    arena.process(make_event(InputEventType::PointerDown, 4, 0, 0, 0.0));
    arena.process(make_event(InputEventType::PointerMove, 4, 20, 0, 0.01));
    if (!expect(arena.claimed(4), "drag claimed")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerCancel, 4, 20, 0, 0.02));
    const auto events = arena.events();
    if (!expect(drag.cancel_calls == 1, "winner canceled") ||
        !expect(events.size() == 1, "one canceled event") ||
        !expect(events[0].phase == GesturePhase::Canceled, "phase is Canceled") ||
        !expect(!arena.tracking(4), "cancel releases the slot")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerUp, 4, 20, 0, 0.03));
    return expect(drag.up_calls == 0, "no up after cancel");
}

bool cancel_without_winner_is_silent() {
    ProbeRecognizer tapper(GestureKind::Tap);
    GestureArena arena;
    arena.add(tapper);
    arena.process(make_event(InputEventType::PointerDown, 5, 0, 0, 0.0));
    arena.process(make_event(InputEventType::PointerCancel, 5, 0, 0, 0.01));
    return expect(arena.events().empty(), "cancel before a claim emits nothing") &&
           expect(tapper.cancel_calls == 0, "candidates are dropped silently") &&
           expect(!arena.tracking(5), "cancel releases the slot");
}

bool blur_cancels_two_pointer_winner_once() {
    ProbeRecognizer pinch(GestureKind::Pinch);
    pinch.multi_pointer_ = true;
    pinch.move_action = RecognizerAction::Claim;
    GestureArena arena;
    arena.add(pinch);
    arena.process(make_event(InputEventType::PointerDown, 6, 0, 0, 0.0));
    arena.process(make_event(InputEventType::PointerDown, 7, 100, 0, 0.01));
    arena.process(make_event(InputEventType::PointerMove, 6, -10, 0, 0.02));
    if (!expect(arena.claimed(6) && arena.claimed(7), "multi-pointer claim wins both ids")) {
        return false;
    }
    InputEvent blur;
    blur.type = InputEventType::Blur;
    blur.timestamp_s = 0.03;
    arena.process(blur);
    return expect(pinch.cancel_calls == 1, "blur cancels the shared winner once") &&
           expect(!arena.tracking(6) && !arena.tracking(7), "blur clears every slot");
}

bool simultaneous_flag() {
    ProbeRecognizer tapper(GestureKind::Tap);
    GestureConfig config;
    config.simultaneous = false;
    GestureArena single(config);
    single.add(tapper);
    single.process(make_event(InputEventType::PointerDown, 8, 0, 0, 0.0));
    single.process(make_event(InputEventType::PointerDown, 9, 10, 0, 0.01));
    if (!expect(single.tracking(8), "first pointer tracked") ||
        !expect(!single.tracking(9), "second pointer ignored when simultaneous is false")) {
        return false;
    }
    single.process(make_event(InputEventType::PointerUp, 8, 0, 0, 0.02));

    GestureConfig shared;
    shared.simultaneous = true;
    GestureArena multi(shared);
    multi.add(tapper);
    multi.process(make_event(InputEventType::PointerDown, 8, 0, 0, 0.0));
    multi.process(make_event(InputEventType::PointerDown, 9, 10, 0, 0.01));
    return expect(multi.active_pointer_count() == 2, "simultaneous arena tracks both pointers");
}

bool capacities_are_bounded() {
    GestureArena arena;
    std::vector<ProbeRecognizer> probes;
    probes.reserve(GestureArena::kMaxRecognizers + 1);
    for (std::size_t i = 0; i < GestureArena::kMaxRecognizers; ++i) {
        probes.emplace_back(GestureKind::Tap);
        if (!expect(arena.add(probes.back()), "recognizer slot accepted")) {
            return false;
        }
    }
    ProbeRecognizer overflow(GestureKind::Tap);
    if (!expect(!arena.add(overflow), "recognizer capacity rejected")) {
        return false;
    }

    GestureConfig config;
    config.simultaneous = true;
    GestureArena pointer_arena(config);
    for (std::uint32_t i = 0; i < GestureArena::kMaxPointers; ++i) {
        pointer_arena.process(make_event(InputEventType::PointerDown, 10 + i, 0, 0, 0.0));
    }
    pointer_arena.process(make_event(InputEventType::PointerDown, 100, 0, 0, 0.0));
    if (!expect(pointer_arena.active_pointer_count() == GestureArena::kMaxPointers,
                "pointer capacity is bounded")) {
        return false;
    }

    BurstRecognizer burst;
    GestureArena burst_arena;
    burst_arena.add(burst);
    burst_arena.process(make_event(InputEventType::PointerDown, 11, 0, 0, 0.0));
    return expect(burst_arena.event_count() == GestureArena::kMaxEvents,
                  "event sink is bounded without allocation");
}

bool clear_drops_state() {
    ProbeRecognizer tapper(GestureKind::Tap);
    tapper.down_action = RecognizerAction::Claim;
    GestureArena arena;
    arena.add(tapper);
    arena.process(make_event(InputEventType::PointerDown, 12, 0, 0, 0.0));
    if (!expect(arena.tracking(12), "tracked before clear")) {
        return false;
    }
    arena.clear();
    if (!expect(!arena.tracking(12), "clear drops pointer state") ||
        !expect(arena.events().empty(), "clear drops the event log")) {
        return false;
    }
    arena.process(make_event(InputEventType::PointerDown, 12, 0, 0, 0.0));
    return expect(arena.tracking(12), "arena still usable after clear");
}

std::vector<std::string> run_script() {
    using tessera::ui::DragRecognizer;
    using tessera::ui::LongPressRecognizer;
    using tessera::ui::TapRecognizer;

    TapRecognizer tapper;
    LongPressRecognizer long_press;
    DragRecognizer drag;
    GestureArena arena;
    arena.add(tapper);
    arena.add(long_press);
    arena.add(drag);

    std::vector<std::string> log;
    const auto record = [&arena, &log]() {
        for (const GestureEvent& event : arena.events()) {
            log.push_back(std::to_string(static_cast<int>(event.kind)) + ":" +
                          std::to_string(static_cast<int>(event.phase)) + ":" +
                          std::to_string(event.pointer_id) + ":" +
                          std::to_string(static_cast<int>(event.x)) + ":" +
                          std::to_string(static_cast<int>(event.dx)));
        }
    };

    arena.process(make_event(InputEventType::PointerDown, 1, 0, 0, 0.0));
    arena.process(make_event(InputEventType::PointerUp, 1, 2, 0, 0.08));
    record(); // tap

    arena.process(make_event(InputEventType::PointerDown, 1, 0, 0, 0.5));
    arena.tick(1.1);
    record(); // long press began
    arena.process(make_event(InputEventType::PointerUp, 1, 0, 0, 1.2));
    record(); // long press ended

    arena.process(make_event(InputEventType::PointerDown, 1, 0, 0, 2.0));
    arena.process(make_event(InputEventType::PointerMove, 1, 20, 0, 2.05));
    record(); // drag began
    arena.process(make_event(InputEventType::PointerMove, 1, 40, 0, 2.1));
    record(); // drag changed
    arena.process(make_event(InputEventType::PointerUp, 1, 45, 0, 2.2));
    record(); // drag ended
    return log;
}

bool deterministic_replay() {
    const std::vector<std::string> first = run_script();
    const std::vector<std::string> second = run_script();
    if (!expect(first.size() == second.size(), "same script produces the same event count") ||
        !expect(!first.empty(), "script produces events")) {
        return false;
    }
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (!expect(first[i] == second[i], "same script produces the same event log")) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (!losers_are_not_polled()) {
        return 1;
    }
    if (!first_claim_wins()) {
        return 2;
    }
    if (!tick_claims()) {
        return 3;
    }
    if (!cancel_emits_canceled()) {
        return 4;
    }
    if (!cancel_without_winner_is_silent()) {
        return 5;
    }
    if (!blur_cancels_two_pointer_winner_once()) {
        return 6;
    }
    if (!simultaneous_flag()) {
        return 7;
    }
    if (!capacities_are_bounded()) {
        return 8;
    }
    if (!clear_drops_state()) {
        return 9;
    }
    if (!deterministic_replay()) {
        return 10;
    }

    std::cout << "gesture_arena: ok\n";
    return 0;
}
