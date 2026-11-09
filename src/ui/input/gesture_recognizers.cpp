// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture_recognizers.hpp"

#include <cmath>

namespace tessera::ui {
namespace {

float distance(float dx, float dy) {
    return std::sqrt(dx * dx + dy * dy);
}

GestureEvent make_event(GestureKind kind, GesturePhase phase, std::uint32_t pointer_id, float x,
                        float y, double timestamp_s) {
    GestureEvent event;
    event.kind = kind;
    event.phase = phase;
    event.pointer_id = pointer_id;
    event.x = x;
    event.y = y;
    event.timestamp_s = timestamp_s;
    return event;
}

double long_press_seconds(const GestureConfig& config) {
    return static_cast<double>(config.long_press_ms) / 1000.0;
}

} // namespace

// Tap / double tap.

RecognizerAction TapRecognizer::on_down(const InputEvent& event, GestureContext) {
    if (pressed_) {
        return RecognizerAction::Ignore;
    }
    pressed_ = true;
    pointer_id_ = event.pointer_id;
    down_x_ = event.x;
    down_y_ = event.y;
    down_time_ = event.timestamp_s;
    return RecognizerAction::Ignore;
}

RecognizerAction TapRecognizer::on_move(const InputEvent& event, GestureContext context) {
    if (!pressed_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    if (distance(event.x - down_x_, event.y - down_y_) > context.config.tap_slop) {
        // Slop exceeded: this is no longer a tap, so a drag/pan recognizer can claim.
        pressed_ = false;
        return RecognizerAction::Reject;
    }
    return RecognizerAction::Ignore;
}

RecognizerAction TapRecognizer::on_up(const InputEvent& event, GestureContext context) {
    if (!pressed_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    const float moved_x = event.x - down_x_;
    const float moved_y = event.y - down_y_;
    const float moved = distance(moved_x, moved_y);
    const bool within_time = event.timestamp_s - down_time_ < long_press_seconds(context.config);
    pressed_ = false;
    if (moved > context.config.tap_slop || !within_time) {
        // A long hold is left to a long-press recognizer.
        return RecognizerAction::Ignore;
    }

    GestureEvent tap = make_event(GestureKind::Tap, GesturePhase::Ended, event.pointer_id, event.x,
                                  event.y, event.timestamp_s);
    const double double_tap_window = static_cast<double>(context.config.double_tap_ms) / 1000.0;
    const bool in_window =
        has_last_tap_ && last_tap_pointer_id_ == event.pointer_id &&
        event.timestamp_s - last_tap_time_ <= double_tap_window &&
        distance(event.x - last_tap_x_, event.y - last_tap_y_) <= context.config.tap_slop;
    if (in_window) {
        has_last_tap_ = false;
        tap.kind = GestureKind::DoubleTap;
        tap.dx = event.x - last_tap_x_;
        tap.dy = event.y - last_tap_y_;
        context.sink.emit(tap);
        return RecognizerAction::Claim;
    }

    has_last_tap_ = true;
    last_tap_pointer_id_ = event.pointer_id;
    last_tap_x_ = event.x;
    last_tap_y_ = event.y;
    last_tap_time_ = event.timestamp_s;
    context.sink.emit(tap);
    return RecognizerAction::Claim;
}

RecognizerAction TapRecognizer::on_tick(double, GestureContext) {
    return RecognizerAction::Ignore;
}

void TapRecognizer::on_cancel(const InputEvent&, GestureContext) {
    // A tap only becomes public on release; canceling an unfinished press emits nothing.
    pressed_ = false;
}

void TapRecognizer::on_claim_lost(std::uint32_t pointer_id) {
    if (pointer_id_ == pointer_id) {
        pressed_ = false;
    }
}

// Long press.

RecognizerAction LongPressRecognizer::on_down(const InputEvent& event, GestureContext) {
    if (pressed_) {
        return RecognizerAction::Ignore;
    }
    pressed_ = true;
    began_ = false;
    pointer_id_ = event.pointer_id;
    down_x_ = event.x;
    down_y_ = event.y;
    last_x_ = event.x;
    last_y_ = event.y;
    down_time_ = event.timestamp_s;
    return RecognizerAction::Ignore;
}

RecognizerAction LongPressRecognizer::on_move(const InputEvent& event, GestureContext context) {
    if (!pressed_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    last_x_ = event.x;
    last_y_ = event.y;
    if (distance(event.x - down_x_, event.y - down_y_) > context.config.tap_slop) {
        pressed_ = false;
        return RecognizerAction::Reject;
    }
    if (!began_ && event.timestamp_s - down_time_ >= long_press_seconds(context.config)) {
        began_ = true;
        context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Began, pointer_id_,
                                     last_x_, last_y_, event.timestamp_s));
        return RecognizerAction::Claim;
    }
    return RecognizerAction::Ignore;
}

RecognizerAction LongPressRecognizer::on_up(const InputEvent& event, GestureContext context) {
    if (!pressed_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    last_x_ = event.x;
    last_y_ = event.y;
    const bool long_enough = event.timestamp_s - down_time_ >= long_press_seconds(context.config);
    const bool already_began = began_;
    pressed_ = false;
    began_ = false;
    if (already_began) {
        context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Ended, pointer_id_,
                                     last_x_, last_y_, event.timestamp_s));
        return RecognizerAction::Claim;
    }
    if (!long_enough) {
        return RecognizerAction::Ignore;
    }
    context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Began, pointer_id_, last_x_,
                                 last_y_, event.timestamp_s));
    context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Ended, pointer_id_, last_x_,
                                 last_y_, event.timestamp_s));
    return RecognizerAction::Claim;
}

RecognizerAction LongPressRecognizer::on_tick(double timestamp_s, GestureContext context) {
    if (!pressed_ || began_) {
        return RecognizerAction::Ignore;
    }
    if (timestamp_s - down_time_ < long_press_seconds(context.config)) {
        return RecognizerAction::Ignore;
    }
    began_ = true;
    context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Began, pointer_id_, last_x_,
                                 last_y_, timestamp_s));
    return RecognizerAction::Claim;
}

void LongPressRecognizer::on_cancel(const InputEvent& event, GestureContext context) {
    if (began_) {
        context.sink.emit(make_event(GestureKind::LongPress, GesturePhase::Canceled, pointer_id_,
                                     last_x_, last_y_, event.timestamp_s));
    }
    pressed_ = false;
    began_ = false;
}

void LongPressRecognizer::on_claim_lost(std::uint32_t pointer_id) {
    if (pointer_id_ == pointer_id) {
        pressed_ = false;
        began_ = false;
    }
}

// Drag.

RecognizerAction DragRecognizer::on_down(const InputEvent& event, GestureContext) {
    if (active_) {
        return RecognizerAction::Ignore;
    }
    active_ = true;
    began_ = false;
    pointer_id_ = event.pointer_id;
    down_x_ = event.x;
    down_y_ = event.y;
    last_x_ = event.x;
    last_y_ = event.y;
    return RecognizerAction::Ignore;
}

RecognizerAction DragRecognizer::on_move(const InputEvent& event, GestureContext context) {
    if (!active_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    last_x_ = event.x;
    last_y_ = event.y;
    GestureEvent drag = make_event(GestureKind::Drag, GesturePhase::Changed, pointer_id_, event.x,
                                   event.y, event.timestamp_s);
    drag.dx = event.x - down_x_;
    drag.dy = event.y - down_y_;
    if (!began_) {
        if (distance(drag.dx, drag.dy) <= context.config.drag_threshold) {
            return RecognizerAction::Ignore;
        }
        began_ = true;
        drag.phase = GesturePhase::Began;
        context.sink.emit(drag);
        return RecognizerAction::Claim;
    }
    context.sink.emit(drag);
    return RecognizerAction::Ignore;
}

RecognizerAction DragRecognizer::on_up(const InputEvent& event, GestureContext context) {
    if (!active_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    last_x_ = event.x;
    last_y_ = event.y;
    const bool was_began = began_;
    active_ = false;
    began_ = false;
    if (!was_began) {
        return RecognizerAction::Ignore;
    }
    GestureEvent drag = make_event(GestureKind::Drag, GesturePhase::Ended, pointer_id_, event.x,
                                   event.y, event.timestamp_s);
    drag.dx = event.x - down_x_;
    drag.dy = event.y - down_y_;
    context.sink.emit(drag);
    return RecognizerAction::Claim;
}

RecognizerAction DragRecognizer::on_tick(double, GestureContext) {
    return RecognizerAction::Ignore;
}

void DragRecognizer::on_cancel(const InputEvent& event, GestureContext context) {
    if (began_) {
        GestureEvent drag = make_event(GestureKind::Drag, GesturePhase::Canceled, pointer_id_,
                                       last_x_, last_y_, event.timestamp_s);
        drag.dx = last_x_ - down_x_;
        drag.dy = last_y_ - down_y_;
        context.sink.emit(drag);
    }
    active_ = false;
    began_ = false;
}

void DragRecognizer::on_claim_lost(std::uint32_t pointer_id) {
    if (pointer_id_ == pointer_id) {
        active_ = false;
        began_ = false;
    }
}

} // namespace tessera::ui
