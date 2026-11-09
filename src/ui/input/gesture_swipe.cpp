// SPDX-License-Identifier: MIT

#include "tessera/ui/gesture_recognizers.hpp"

#include <cmath>

namespace tessera::ui {
namespace {

// Deterministic swipe thresholds. Velocity is measured over the most recent event-time window, so
// the same script always produces the same claim.
constexpr float kMinVelocity = 500.0f;   // logical px/s
constexpr double kVelocityWindowS = 0.1; // seconds

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

} // namespace

void SwipeRecognizer::reset() {
    active_ = false;
    began_ = false;
    history_count_ = 0;
}

void SwipeRecognizer::push_sample(const InputEvent& event) {
    const Sample sample{event.timestamp_s, event.x, event.y};
    if (history_count_ < history_.size()) {
        history_[history_count_++] = sample;
        return;
    }
    for (std::size_t i = 1; i < history_.size(); ++i) {
        history_[i - 1] = history_[i];
    }
    history_[history_.size() - 1] = sample;
}

float SwipeRecognizer::speed() const {
    if (history_count_ < 2) {
        return 0.0f;
    }
    const Sample& latest = history_[history_count_ - 1];
    std::size_t first = history_count_ - 1;
    for (std::size_t i = 0; i < history_count_; ++i) {
        if (latest.timestamp_s - history_[i].timestamp_s <= kVelocityWindowS) {
            first = i;
            break;
        }
    }
    const Sample& oldest = history_[first];
    const double dt = latest.timestamp_s - oldest.timestamp_s;
    if (dt <= 0.0) {
        return 0.0f;
    }
    return distance(latest.x - oldest.x, latest.y - oldest.y) / static_cast<float>(dt);
}

RecognizerAction SwipeRecognizer::on_down(const InputEvent& event, GestureContext) {
    if (active_) {
        return RecognizerAction::Ignore;
    }
    reset();
    active_ = true;
    pointer_id_ = event.pointer_id;
    down_x_ = event.x;
    down_y_ = event.y;
    push_sample(event);
    return RecognizerAction::Ignore;
}

RecognizerAction SwipeRecognizer::on_move(const InputEvent& event, GestureContext context) {
    if (!active_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    push_sample(event);
    GestureEvent swipe = make_event(GestureKind::Swipe, GesturePhase::Changed, pointer_id_, event.x,
                                    event.y, event.timestamp_s);
    swipe.dx = event.x - down_x_;
    swipe.dy = event.y - down_y_;
    if (!began_) {
        const bool far_enough = distance(swipe.dx, swipe.dy) >= context.config.drag_threshold;
        if (!far_enough || speed() < kMinVelocity) {
            return RecognizerAction::Ignore;
        }
        began_ = true;
        swipe.phase = GesturePhase::Began;
        context.sink.emit(swipe);
        return RecognizerAction::Claim;
    }
    context.sink.emit(swipe);
    return RecognizerAction::Ignore;
}

RecognizerAction SwipeRecognizer::on_up(const InputEvent& event, GestureContext context) {
    if (!active_ || event.pointer_id != pointer_id_) {
        return RecognizerAction::Ignore;
    }
    push_sample(event);
    const bool was_began = began_;
    const float dx = event.x - down_x_;
    const float dy = event.y - down_y_;
    reset();
    if (!was_began) {
        return RecognizerAction::Ignore;
    }
    GestureEvent swipe = make_event(GestureKind::Swipe, GesturePhase::Ended, pointer_id_, event.x,
                                    event.y, event.timestamp_s);
    swipe.dx = dx;
    swipe.dy = dy;
    context.sink.emit(swipe);
    return RecognizerAction::Claim;
}

RecognizerAction SwipeRecognizer::on_tick(double, GestureContext) {
    return RecognizerAction::Ignore;
}

void SwipeRecognizer::on_cancel(const InputEvent& event, GestureContext context) {
    if (began_ && history_count_ > 0) {
        const Sample& latest = history_[history_count_ - 1];
        GestureEvent swipe = make_event(GestureKind::Swipe, GesturePhase::Canceled, pointer_id_,
                                        latest.x, latest.y, event.timestamp_s);
        swipe.dx = latest.x - down_x_;
        swipe.dy = latest.y - down_y_;
        context.sink.emit(swipe);
    }
    reset();
}

void SwipeRecognizer::on_claim_lost(std::uint32_t pointer_id) {
    if (pointer_id_ == pointer_id) {
        reset();
    }
}

} // namespace tessera::ui
