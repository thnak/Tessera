// SPDX-License-Identifier: MIT

#include "tessera/ui/input_pump.hpp"

#include <stdexcept>

namespace tessera::ui {

InputPump::InputPump(InputPumpConfig config)
    : config_(config),
      queue_(config.queue_capacity),
      sampler_(config.max_events_per_tick) {
    if (config_.max_events_per_tick == 0) {
        throw std::invalid_argument("input pump max_events_per_tick must be greater than zero");
    }
}

InputPumpStats InputPump::pump(Node& root, runtime::RuntimeLoop& loop) {
    InputPumpStats stats;

    sampler_.sample(queue_, config_.max_events_per_tick);
    for (const InputEvent& event : sampler_.events()) {
        ++stats.consumed;
        const InputDispatchResult result = router_.dispatch(root, event);
        if (result.dispatched) {
            ++stats.dispatched;
        } else {
            ++stats.dropped;
        }
        if (gestures_enabled_) {
            // One arena feed per dispatched input event, in queue order. The target id is only
            // meaningful (and only stored) for PointerDown; the arena ignores it otherwise.
            gesture_arena_.process(event, static_cast<std::uint64_t>(result.target_id));
            deliver_gestures();
        }
    }
    stats.overflowed = queue_.overflowed();

    total_.consumed += stats.consumed;
    total_.dispatched += stats.dispatched;
    total_.dropped += stats.dropped;
    total_.overflowed = total_.overflowed || stats.overflowed;

    if (stats.consumed > 0) {
        loop.request_frame();
    }
    return stats;
}

void InputPump::enable_gestures(const GestureConfig& config) {
    gesture_arena_.set_config(config);
    if (gestures_enabled_) {
        return;
    }
    // Registration order is the v1 priority: a held contact becomes a long press before the tap
    // fallthrough, a two-pointer gesture claims both contacts before single-pointer drag/swipe
    // (its handlers ignore a lone pointer), and a fast swipe wins over a plain drag.
    gesture_arena_.add(long_press_recognizer_);
    gesture_arena_.add(tap_recognizer_);
    gesture_arena_.add(pinch_rotate_recognizer_);
    gesture_arena_.add(swipe_recognizer_);
    gesture_arena_.add(drag_recognizer_);
    gestures_enabled_ = true;
}

void InputPump::set_gesture_callback(GestureCallback callback, void* user) {
    gesture_callback_ = callback;
    gesture_user_ = user;
}

void InputPump::tick(double timestamp_s) {
    gesture_arena_.tick(timestamp_s);
    deliver_gestures();
}

void InputPump::deliver_gestures() {
    if (gesture_callback_ == nullptr) {
        return;
    }
    for (const GestureEvent& event : gesture_arena_.events()) {
        gesture_callback_(gesture_user_, event);
    }
}

void InputPump::reset() {
    InputEvent event;
    while (queue_.pop(event)) {
    }
    queue_.clear_overflow();
    sampler_.sample(queue_);
    router_.reset();
    gesture_arena_.reset();
}

} // namespace tessera::ui
