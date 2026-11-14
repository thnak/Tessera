// SPDX-License-Identifier: MIT

#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/scroll.hpp"

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
        if (input_filter_ != nullptr && input_filter_(input_filter_user_, root, event)) {
            // Swallowed before dispatch: it counts as handled, and the arena never sees it.
            ++stats.dispatched;
            continue;
        }
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
            // Scroll handoff runs before the callback so observers see the post-scroll offsets.
            apply_gesture_pan(root);
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
    // Time-driven recognizers only (for example long press); a drag emits on pointer moves, which
    // pump() feeds with the scene root available for the scroll handoff.
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

void InputPump::apply_gesture_pan(Node& root) {
    for (const GestureEvent& event : gesture_arena_.events()) {
        if (event.kind != GestureKind::Drag) {
            continue;
        }
        PanSlot* slot = nullptr;
        for (PanSlot& candidate : pan_slots_) {
            if (candidate.active && candidate.pointer_id == event.pointer_id) {
                slot = &candidate;
                break;
            }
        }
        switch (event.phase) {
        case GesturePhase::Began: {
            if (slot == nullptr) {
                for (PanSlot& candidate : pan_slots_) {
                    if (!candidate.active) {
                        slot = &candidate;
                        break;
                    }
                }
            }
            if (slot == nullptr) {
                // Bounded slots exhausted: this drag stays with the other consumers.
                continue;
            }
            slot->pointer_id = event.pointer_id;
            slot->target_id = event.target_id;
            slot->last_dx = event.dx;
            slot->last_dy = event.dy;
            // Only a press over a scroll container turns the drag into scroll. The arena claims the
            // drag regardless (it has no scene knowledge), so a non-scrollable press still reaches
            // pointer handlers and the gesture callback; only the scroll side is declined.
            slot->active = scroll_target_for_id(root, event.target_id) != nullptr;
            break;
        }
        case GesturePhase::Changed: {
            if (slot == nullptr || !slot->active) {
                break;
            }
            Node* const container = scroll_target_for_id(root, slot->target_id);
            if (container == nullptr) {
                // The pressed subtree was removed; stop the pan instead of dereferencing it.
                slot->active = false;
                break;
            }
            const float step_x = event.dx - slot->last_dx;
            const float step_y = event.dy - slot->last_dy;
            slot->last_dx = event.dx;
            slot->last_dy = event.dy;
            // Gesture dx/dy are cumulative from the press point, so scroll by the increment.
            // Content follows the pointer, hence the opposite sign. scroll_by clamps every move; at
            // an edge the claim is kept and the excess delta is discarded (no chaining in this
            // slice).
            scroll_by(*container, -step_x, -step_y);
            break;
        }
        case GesturePhase::Ended: {
            if (slot == nullptr) {
                break;
            }
            if (slot->active) {
                // Apply the tail of the drag (the pointer may have moved since the last move
                // event), then release the slot. Momentum/fling is a later slice.
                if (Node* const container = scroll_target_for_id(root, slot->target_id)) {
                    scroll_by(*container, -(event.dx - slot->last_dx), -(event.dy - slot->last_dy));
                }
            }
            slot->active = false;
            slot->pointer_id = 0;
            slot->target_id = 0;
            break;
        }
        case GesturePhase::Canceled: {
            if (slot != nullptr) {
                // Cancellation stops immediately; the remaining displacement is dropped.
                slot->active = false;
                slot->pointer_id = 0;
                slot->target_id = 0;
            }
            break;
        }
        }
    }
}

void InputPump::clear_pan_slots() {
    pan_slots_ = {};
}

void InputPump::reset() {
    InputEvent event;
    while (queue_.pop(event)) {
    }
    queue_.clear_overflow();
    sampler_.sample(queue_);
    router_.reset();
    gesture_arena_.reset();
    clear_pan_slots();
}

} // namespace tessera::ui
