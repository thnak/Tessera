// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/gesture.hpp"
#include "tessera/ui/gesture_recognizers.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui {

struct InputPumpConfig {
    // Capacity of the SPSC queue shared with the platform translator.
    std::size_t queue_capacity = 256;
    // Hard bound on events consumed per pump() call; the rest stay queued for the next tick.
    std::size_t max_events_per_tick = 256;
};

// Counters for one pump() call; InputPump::total() accumulates them.
struct InputPumpStats {
    std::uint64_t consumed = 0;
    std::uint64_t dispatched = 0;
    // Consumed but not delivered: no hit target, no focused node, or a lifecycle event
    // (PointerCancel/Focus/Blur) that only updates router state.
    std::uint64_t dropped = 0;
    // True when the queue has overflowed since the last reset; the platform reports the loss.
    bool overflowed = false;
};

// UI-thread input pump: drains the bounded queue under a per-tick budget, coalesces adjacent
// pointer moves, dispatches through the InputRouter, and wakes a render-on-demand RuntimeLoop.
//
// No blocking, no I/O, and no allocation once the queue and sampler are warm (both preallocate in
// the constructor). Handlers run synchronously on the calling (UI) thread. The pump owns the
// queue the platform writes into; hand it to hal::Window::set_input_queue().
//
// Optional gesture pipeline (opt-in, additive): `enable_gestures` registers the built-in
// recognizers on an owned `GestureArena`, and every pumped input event is then fed to the arena
// exactly once, in queue order, after the router dispatched it. Pointer events keep dispatching
// exactly as before; gesture claims never suppress pointer events in v1. Each arena event is
// delivered synchronously to the installed bounded callback, which is invoked only from `pump`
// and `tick` (never during construction or from another thread). The arena's fixed 32-event sink
// bounds one call; extra events beyond that bound are dropped, and no per-event allocation is
// performed. Two-pointer gestures keep per-pointer ids; events for one pointer are delivered in
// queue order and are never reordered relative to that pointer.
//
// Gesture pan scrolling (part of the same opt-in pipeline): a `Drag` claim whose PointerDown target
// has a scroll container scrolls the innermost scrollable ancestor by the per-move pointer delta in
// logical pixels, with the scroll model's clamp applied on every move. Content follows the pointer,
// so a downward drag decreases `scroll_y`. At a clamped edge the gesture keeps its claim and the
// excess delta is discarded (no nested chaining, no momentum/fling). `PointerCancel` ends the pan
// and drops the remaining delta. A press over a non-scrollable chain produces no scroll: the drag
// still reaches the gesture callback and the pointer handlers, because the arena has no scene
// knowledge and pointer events are never suppressed. Wheel routing is untouched.
class InputPump {
public:
    // Gesture delivery callback: a plain function pointer plus opaque user data keeps the
    // per-event path allocation-free (no std::function target to store).
    using GestureCallback = void (*)(void* user, const GestureEvent& event);

    explicit InputPump(InputPumpConfig config = {});

    InputPump(const InputPump&) = delete;
    InputPump& operator=(const InputPump&) = delete;
    InputPump(InputPump&&) = delete;
    InputPump& operator=(InputPump&&) = delete;

    [[nodiscard]] InputQueue& queue() {
        return queue_;
    }
    [[nodiscard]] const InputQueue& queue() const {
        return queue_;
    }
    [[nodiscard]] InputRouter& router() {
        return router_;
    }
    [[nodiscard]] const InputRouter& router() const {
        return router_;
    }

    // Consumes at most max_events_per_tick events, dispatches each through the router (and the
    // gesture arena when enabled), and calls loop.request_frame() when at least one event was
    // consumed (so idle loops wake even when the event is dropped by hit testing).
    InputPumpStats pump(Node& root, runtime::RuntimeLoop& loop);

    [[nodiscard]] InputPumpStats total() const {
        return total_;
    }

    // Turns on the built-in gesture pipeline with `config` (LongPress, Tap/DoubleTap, Swipe, Drag,
    // PinchRotate registrations in that order). Idempotent: a second call only updates the config
    // of the existing arena. Gesture targets are the NodeId of the deepest hit node at
    // PointerDown.
    void enable_gestures(const GestureConfig& config = {});
    [[nodiscard]] bool gestures_enabled() const {
        return gestures_enabled_;
    }
    // Installs the callback that receives every gesture event produced by a pump()/tick() call.
    // `nullptr` clears it. The callback runs synchronously on the pumping thread.
    void set_gesture_callback(GestureCallback callback, void* user = nullptr);

    // A hook that sees every event before the router does (overlay dismissal, issue #302). It
    // returns true to consume the event: the router and the gesture arena never see it. `nullptr`
    // clears it. It runs synchronously on the pumping thread and must not block.
    using InputFilter = bool (*)(void* user, Node& root, const InputEvent& event);
    void set_input_filter(InputFilter filter, void* user = nullptr) {
        input_filter_ = filter;
        input_filter_user_ = user;
    }
    [[nodiscard]] GestureArena& gesture_arena() {
        return gesture_arena_;
    }
    [[nodiscard]] const GestureArena& gesture_arena() const {
        return gesture_arena_;
    }

    // Advances time-driven recognizers (for example long press) with simulated time in seconds and
    // delivers this tick's gesture events. The application/frame path calls this once per tick
    // with the sampled loop time; the pump never reads the wall clock. No effect when gestures are
    // disabled. Callback delivery is bounded by GestureArena::kMaxEvents per call.
    void tick(double timestamp_s);

    // Drops queued events, clears the overflow flag, and resets capture/focus plus any in-flight
    // gesture state (no gesture events are emitted by reset).
    void reset();

private:
    // One gesture-pan scroll slot per arena pointer (fixed capacity, no allocation).
    struct PanSlot {
        std::uint32_t pointer_id = 0;
        std::uint64_t target_id = 0;
        float last_dx = 0.0f;
        float last_dy = 0.0f;
        bool active = false;
    };

    void deliver_gestures();
    // Applies the current arena event batch to the scroll model (see the class comment). Runs in
    // `pump` only: a NodeId is resolved against the root passed there, and time-driven ticks never
    // emit drag events.
    void apply_gesture_pan(Node& root);
    void clear_pan_slots();

    InputPumpConfig config_;
    InputQueue queue_;
    InputSampler sampler_;
    InputRouter router_;
    InputPumpStats total_;

    bool gestures_enabled_ = false;
    InputFilter input_filter_ = nullptr;
    void* input_filter_user_ = nullptr;
    GestureCallback gesture_callback_ = nullptr;
    void* gesture_user_ = nullptr;
    GestureArena gesture_arena_;
    LongPressRecognizer long_press_recognizer_;
    TapRecognizer tap_recognizer_;
    SwipeRecognizer swipe_recognizer_;
    DragRecognizer drag_recognizer_;
    PinchRotateRecognizer pinch_rotate_recognizer_;
    std::array<PanSlot, GestureArena::kMaxPointers> pan_slots_{};
};

} // namespace tessera::ui
