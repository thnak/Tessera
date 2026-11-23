// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// Long-press and drag-to-select for the Android shell (issue #189, docs/guides/android-lane.md
// section 3.14).
//
// A tap places the caret (section 3.13). The other half of the gesture is a long-press that starts
// a selection and a drag that extends it, and the platform does not report either one: it reports
// raw DOWN/MOVE/UP samples with timestamps, and the app decides what they mean. That decision is a
// small state machine, and it is PURE - no NDK, no JNI, no device - so the thresholds and the
// transitions are unit-tested on every lane instead of only by a finger on a phone.
//
// The thresholds are the platform's own, so a long-press feels the same as in every other app:
// `ViewConfiguration.getLongPressTimeout()` is 500 ms and `getScaledTouchSlop()` is 8 dp, which is
// 8 * density physical pixels.

namespace tessera::hal::android {

// The gesture's timing and distance thresholds, in the units the samples use.
struct SelectionGestureConfig {
    // How long a finger must stay down before the press becomes a selection, in seconds.
    // `ViewConfiguration.getLongPressTimeout()` is 500 ms.
    double long_press_seconds = 0.5;
    // How far the finger may move before the press stops being a press, in LOGICAL pixels. The
    // platform's `getScaledTouchSlop()` is 8 dp, so the caller scales it by the density.
    float touch_slop = 8.0f;
};

// What the caller should do with this sample.
enum class SelectionGestureAction : std::uint8_t {
    // Nothing: the sample is part of a gesture the caller already handled, or it is not a press.
    None,
    // The press became a long-press: start a selection at the press point.
    BeginSelection,
    // The finger moved while selecting: extend the selection to the new point.
    ExtendSelection,
    // The gesture ended. The caller keeps whatever selection it has.
    EndSelection,
};

// The state machine. One instance per field, owned by the shell.
//
// It is deliberately not a `MotionGesture`: that one tracks pointer identity for the input queue,
// and this one tracks what the press MEANS. Keeping them apart means a change to the selection
// thresholds cannot affect the queue's capture bookkeeping.
class SelectionGesture {
public:
    // Feeds one sample. `x`/`y` are in LOGICAL pixels (the caller has already divided by the
    // density), and `time_seconds` is the sample's own timestamp - never a wall clock, so a test
    // drives it deterministically.
    //
    // `down` is true for the sample that starts a press, `up` for the one that ends it, and false
    // for a move. The caller maps the platform's action to these three, because the platform has
    // more actions than the gesture cares about (a cancel ends it exactly like an up).
    [[nodiscard]] SelectionGestureAction feed(bool down, bool up, float x, float y,
                                              double time_seconds,
                                              const SelectionGestureConfig& config) noexcept;

    // The timeout half of the state machine. A finger that holds still produces no samples at all,
    // so the caller ticks this from its frame loop (or a posted callback) with the current time.
    // Returns `BeginSelection` exactly once, when a held press crosses the threshold.
    [[nodiscard]] SelectionGestureAction tick(double time_seconds,
                                              const SelectionGestureConfig& config) noexcept;

    // Forgets the gesture. Called when the field loses focus or the platform cancels it, so a
    // stale press cannot turn the next tap into a selection.
    void reset() noexcept;

    [[nodiscard]] bool selecting() const noexcept {
        return selecting_;
    }
    // True while a press is down and has not yet become a selection. The caller uses it to decide
    // whether a move is a scroll or a selection drag.
    [[nodiscard]] bool pressing() const noexcept {
        return pressing_;
    }

private:
    bool pressing_ = false;
    bool selecting_ = false;
    float press_x_ = 0.0f;
    float press_y_ = 0.0f;
    double press_time_ = 0.0;
    // The last point the selection was extended to, so a move that did not cross a threshold does
    // not republish the same selection every frame.
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
};

} // namespace tessera::hal::android
