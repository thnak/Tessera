// SPDX-License-Identifier: MIT

#include "android_selection_gesture.hpp"

#include <cmath>

namespace tessera::hal::android {

namespace {

float distance(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

SelectionGestureAction SelectionGesture::feed(bool down, bool up, float x, float y,
                                              double time_seconds,
                                              const SelectionGestureConfig& config) noexcept {
    if (down) {
        // A new press always starts from scratch: a previous gesture that never saw its up (the
        // platform dropped it) must not leak into this one.
        pressing_ = true;
        selecting_ = false;
        press_x_ = x;
        press_y_ = y;
        press_time_ = time_seconds;
        last_x_ = x;
        last_y_ = y;
        return SelectionGestureAction::None;
    }
    if (up) {
        const bool was_selecting = selecting_;
        pressing_ = false;
        selecting_ = false;
        // An up ends a selection the caller already has; it never starts one. Reporting it lets the
        // caller stop tracking the drag.
        return was_selecting ? SelectionGestureAction::EndSelection : SelectionGestureAction::None;
    }
    // A move.
    if (!pressing_ && !selecting_) {
        return SelectionGestureAction::None;
    }
    if (selecting_) {
        // A drag extends the selection. The threshold here is the slop, not zero: a finger that
        // jitters by a pixel must not republish the selection every frame.
        if (distance(x, y, last_x_, last_y_) < config.touch_slop) {
            return SelectionGestureAction::None;
        }
        last_x_ = x;
        last_y_ = y;
        return SelectionGestureAction::ExtendSelection;
    }
    // Still pressing. A move past the slop before the long-press timeout means the user is
    // scrolling, not selecting, so the press is abandoned - the same rule every platform applies.
    if (distance(x, y, press_x_, press_y_) > config.touch_slop) {
        pressing_ = false;
        return SelectionGestureAction::None;
    }
    // A move that stayed inside the slop: the press is still a press. The timeout is checked here
    // too, so a sample that arrives after it turns the press into a selection without waiting for
    // the caller's tick.
    if (time_seconds - press_time_ >= config.long_press_seconds) {
        pressing_ = false;
        selecting_ = true;
        last_x_ = x;
        last_y_ = y;
        return SelectionGestureAction::BeginSelection;
    }
    return SelectionGestureAction::None;
}

SelectionGestureAction SelectionGesture::tick(double time_seconds,
                                              const SelectionGestureConfig& config) noexcept {
    if (!pressing_ || selecting_) {
        return SelectionGestureAction::None;
    }
    if (time_seconds - press_time_ < config.long_press_seconds) {
        return SelectionGestureAction::None;
    }
    pressing_ = false;
    selecting_ = true;
    last_x_ = press_x_;
    last_y_ = press_y_;
    return SelectionGestureAction::BeginSelection;
}

void SelectionGesture::reset() noexcept {
    pressing_ = false;
    selecting_ = false;
    press_x_ = 0.0f;
    press_y_ = 0.0f;
    press_time_ = 0.0;
    last_x_ = 0.0f;
    last_y_ = 0.0f;
}

} // namespace tessera::hal::android
