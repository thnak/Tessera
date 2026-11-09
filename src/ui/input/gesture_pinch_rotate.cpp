// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture_recognizers.hpp"

#include <cmath>

namespace tessera::ui {
namespace {

constexpr float kRotationThreshold = 0.15f; // radians
constexpr float kTwoPi = 6.283185307179586f;

float distance(float dx, float dy) {
    return std::sqrt(dx * dx + dy * dy);
}

float angle_of(float dx, float dy) {
    return std::atan2(dy, dx);
}

float wrap_angle(float angle) {
    return std::remainder(angle, kTwoPi);
}

GestureEvent make_event(GestureKind kind, GesturePhase phase, std::uint32_t pointer_id,
                        std::uint32_t secondary_pointer_id, float x, float y, float scale,
                        float rotation, double timestamp_s) {
    GestureEvent event;
    event.kind = kind;
    event.phase = phase;
    event.pointer_id = pointer_id;
    event.secondary_pointer_id = secondary_pointer_id;
    event.x = x;
    event.y = y;
    event.scale = scale;
    event.rotation = rotation;
    event.timestamp_s = timestamp_s;
    return event;
}

} // namespace

RecognizerAction PinchRotateRecognizer::on_down(const InputEvent& event, GestureContext) {
    if (!has_first_) {
        has_first_ = true;
        first_id_ = event.pointer_id;
        first_x_ = event.x;
        first_y_ = event.y;
        return RecognizerAction::Ignore;
    }
    if (!has_second_ && event.pointer_id != first_id_) {
        has_second_ = true;
        began_ = false;
        second_id_ = event.pointer_id;
        second_x_ = event.x;
        second_y_ = event.y;
        base_distance_ = distance(second_x_ - first_x_, second_y_ - first_y_);
        base_angle_ = angle_of(second_x_ - first_x_, second_y_ - first_y_);
    }
    return RecognizerAction::Ignore;
}

RecognizerAction PinchRotateRecognizer::on_move(const InputEvent& event, GestureContext context) {
    if (!has_first_) {
        return RecognizerAction::Ignore;
    }
    if (event.pointer_id == first_id_) {
        first_x_ = event.x;
        first_y_ = event.y;
    } else if (has_second_ && event.pointer_id == second_id_) {
        second_x_ = event.x;
        second_y_ = event.y;
    } else {
        return RecognizerAction::Ignore;
    }
    if (!has_second_ || base_distance_ <= 0.0f) {
        return RecognizerAction::Ignore;
    }

    const float dist = distance(second_x_ - first_x_, second_y_ - first_y_);
    const float scale = dist / base_distance_;
    const float rotation =
        wrap_angle(angle_of(second_x_ - first_x_, second_y_ - first_y_) - base_angle_);
    const float mid_x = (first_x_ + second_x_) * 0.5f;
    const float mid_y = (first_y_ + second_y_) * 0.5f;

    if (!began_) {
        const bool moved_enough = std::fabs(dist - base_distance_) >= context.config.drag_threshold;
        if (!moved_enough && std::fabs(rotation) < kRotationThreshold) {
            return RecognizerAction::Ignore;
        }
        began_ = true;
        context.sink.emit(make_event(GestureKind::Pinch, GesturePhase::Began, first_id_, second_id_,
                                     mid_x, mid_y, scale, rotation, event.timestamp_s));
        context.sink.emit(make_event(GestureKind::Rotate, GesturePhase::Began, first_id_,
                                     second_id_, mid_x, mid_y, scale, rotation, event.timestamp_s));
        return RecognizerAction::Claim;
    }

    context.sink.emit(make_event(GestureKind::Pinch, GesturePhase::Changed, first_id_, second_id_,
                                 mid_x, mid_y, scale, rotation, event.timestamp_s));
    context.sink.emit(make_event(GestureKind::Rotate, GesturePhase::Changed, first_id_, second_id_,
                                 mid_x, mid_y, scale, rotation, event.timestamp_s));
    return RecognizerAction::Ignore;
}

RecognizerAction PinchRotateRecognizer::on_up(const InputEvent& event, GestureContext context) {
    if (!has_first_ && !has_second_) {
        return RecognizerAction::Ignore;
    }
    if (event.pointer_id == first_id_) {
        first_x_ = event.x;
        first_y_ = event.y;
    } else if (has_second_ && event.pointer_id == second_id_) {
        second_x_ = event.x;
        second_y_ = event.y;
    }
    const bool was_began = began_;
    const std::uint32_t first_id = first_id_;
    const std::uint32_t second_id = second_id_;
    const float mid_x = (first_x_ + second_x_) * 0.5f;
    const float mid_y = (first_y_ + second_y_) * 0.5f;
    const float dist = distance(second_x_ - first_x_, second_y_ - first_y_);
    const float scale = base_distance_ > 0.0f ? dist / base_distance_ : 1.0f;
    const float rotation =
        wrap_angle(angle_of(second_x_ - first_x_, second_y_ - first_y_) - base_angle_);
    began_ = false;
    has_first_ = false;
    has_second_ = false;
    if (!was_began) {
        return RecognizerAction::Ignore;
    }
    context.sink.emit(make_event(GestureKind::Pinch, GesturePhase::Ended, first_id, second_id,
                                 mid_x, mid_y, scale, rotation, event.timestamp_s));
    context.sink.emit(make_event(GestureKind::Rotate, GesturePhase::Ended, first_id, second_id,
                                 mid_x, mid_y, scale, rotation, event.timestamp_s));
    return RecognizerAction::Claim;
}

RecognizerAction PinchRotateRecognizer::on_tick(double, GestureContext) {
    return RecognizerAction::Ignore;
}

void PinchRotateRecognizer::on_cancel(const InputEvent& event, GestureContext context) {
    if (began_) {
        const float mid_x = (first_x_ + second_x_) * 0.5f;
        const float mid_y = (first_y_ + second_y_) * 0.5f;
        context.sink.emit(make_event(GestureKind::Pinch, GesturePhase::Canceled, first_id_,
                                     second_id_, mid_x, mid_y, 1.0f, 0.0f, event.timestamp_s));
        context.sink.emit(make_event(GestureKind::Rotate, GesturePhase::Canceled, first_id_,
                                     second_id_, mid_x, mid_y, 1.0f, 0.0f, event.timestamp_s));
    }
    began_ = false;
    has_first_ = false;
    has_second_ = false;
}

void PinchRotateRecognizer::on_claim_lost(std::uint32_t pointer_id) {
    if (!has_first_ || pointer_id == first_id_ || pointer_id == second_id_) {
        began_ = false;
        has_first_ = false;
        has_second_ = false;
    }
}

} // namespace tessera::ui
