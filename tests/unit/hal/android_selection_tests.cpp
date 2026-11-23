// SPDX-License-Identifier: MIT
//
// Long-press and drag-to-select for the Android shell (issue #189, docs/guides/android-lane.md
// section 3.14).
//
// The platform reports raw DOWN/MOVE/UP samples with timestamps; what they MEAN is the app's
// decision. That decision is a small state machine, and it is pinned here rather than by a finger
// on a phone: the thresholds (500 ms, 8 dp) and the transitions are the whole behaviour, and a
// wrong one turns every scroll into a selection or makes a long-press never fire.
//
// The rules are pure over plain numbers, so they run on every lane with no NDK, no JNI and no
// device.

#include "android_selection_gesture.hpp"

#include <iostream>

namespace {

namespace android = tessera::hal::android;
using android::SelectionGesture;
using android::SelectionGestureAction;
using android::SelectionGestureConfig;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

constexpr SelectionGestureConfig kConfig{0.5, 8.0f};

} // namespace

int main() {
    // --- A tap is not a selection ------------------------------------------------------------
    //
    // Down and up inside the timeout: no selection, and the up reports nothing because there is
    // nothing to end.
    {
        SelectionGesture gesture;
        check(gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig) ==
                  SelectionGestureAction::None,
              "a down starts no selection");
        check(gesture.pressing(), "a down starts a press");
        check(!gesture.selecting(), "a down does not select");
        check(gesture.feed(false, true, 100.0f, 100.0f, 0.1, kConfig) ==
                  SelectionGestureAction::None,
              "an up before the timeout ends nothing");
        check(!gesture.pressing() && !gesture.selecting(), "the gesture is over");
    }

    // --- A held press becomes a selection ----------------------------------------------------
    //
    // A finger that holds still produces no samples, so the caller ticks the clock. The tick fires
    // exactly once.
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        check(gesture.tick(0.3, kConfig) == SelectionGestureAction::None,
              "a tick before the timeout does nothing");
        check(gesture.pressing(), "the press is still a press");
        check(gesture.tick(0.5, kConfig) == SelectionGestureAction::BeginSelection,
              "a tick at the timeout begins a selection");
        check(gesture.selecting(), "the gesture is selecting");
        check(!gesture.pressing(), "a selecting gesture is no longer pressing");
        check(gesture.tick(0.9, kConfig) == SelectionGestureAction::None,
              "a second tick does not begin a second selection");
    }

    // --- A move past the slop before the timeout abandons the press --------------------------
    //
    // That is a scroll, not a selection. Without this rule every drag would select instead of
    // scrolling.
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        check(gesture.feed(false, false, 130.0f, 100.0f, 0.1, kConfig) ==
                  SelectionGestureAction::None,
              "a move past the slop reports nothing");
        check(!gesture.pressing(), "a move past the slop abandons the press");
        check(!gesture.selecting(), "a move past the slop does not select");
        check(gesture.tick(1.0, kConfig) == SelectionGestureAction::None,
              "an abandoned press never becomes a selection");
    }

    // --- A move inside the slop keeps the press alive ----------------------------------------
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        check(gesture.feed(false, false, 104.0f, 103.0f, 0.1, kConfig) ==
                  SelectionGestureAction::None,
              "a small move reports nothing");
        check(gesture.pressing(), "a small move keeps the press");
        check(gesture.tick(0.5, kConfig) == SelectionGestureAction::BeginSelection,
              "the press still becomes a selection");
    }

    // --- A drag after the selection extends it -----------------------------------------------
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        check(gesture.tick(0.5, kConfig) == SelectionGestureAction::BeginSelection,
              "the press became a selection");
        check(gesture.feed(false, false, 140.0f, 100.0f, 0.6, kConfig) ==
                  SelectionGestureAction::ExtendSelection,
              "a drag extends the selection");
        // A jitter smaller than the slop must not republish: the caller would repaint for nothing.
        check(gesture.feed(false, false, 142.0f, 101.0f, 0.65, kConfig) ==
                  SelectionGestureAction::None,
              "a jitter inside the slop does not extend");
        check(gesture.feed(false, false, 180.0f, 100.0f, 0.7, kConfig) ==
                  SelectionGestureAction::ExtendSelection,
              "a real drag extends again");
    }

    // --- An up ends the selection ------------------------------------------------------------
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        (void)gesture.tick(0.5, kConfig);
        check(gesture.feed(false, true, 140.0f, 100.0f, 0.6, kConfig) ==
                  SelectionGestureAction::EndSelection,
              "an up ends the selection");
        check(!gesture.selecting(), "the gesture is over");
        // A move after the up is not part of the gesture.
        check(gesture.feed(false, false, 200.0f, 100.0f, 0.7, kConfig) ==
                  SelectionGestureAction::None,
              "a move after the up reports nothing");
    }

    // --- A new down resets a gesture that never saw its up -----------------------------------
    //
    // The platform can drop an up (a cancel, a focus change). A stale press must not leak into the
    // next gesture, or the next tap would select instead of placing the caret.
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        (void)gesture.tick(0.5, kConfig);
        check(gesture.selecting(), "the first gesture is selecting");
        check(gesture.feed(true, false, 300.0f, 300.0f, 1.0, kConfig) ==
                  SelectionGestureAction::None,
              "a new down starts over");
        check(gesture.pressing() && !gesture.selecting(), "the new gesture is a fresh press");
        check(gesture.tick(1.2, kConfig) == SelectionGestureAction::None,
              "the new press has its own timeout");
        check(gesture.tick(1.5, kConfig) == SelectionGestureAction::BeginSelection,
              "the new press becomes a selection at its own timeout");
    }

    // --- reset() forgets everything ----------------------------------------------------------
    {
        SelectionGesture gesture;
        (void)gesture.feed(true, false, 100.0f, 100.0f, 0.0, kConfig);
        gesture.reset();
        check(!gesture.pressing() && !gesture.selecting(), "reset forgets the gesture");
        check(gesture.tick(1.0, kConfig) == SelectionGestureAction::None,
              "a reset press never becomes a selection");
    }

    // --- A move with no press in flight is ignored -------------------------------------------
    {
        SelectionGesture gesture;
        check(gesture.feed(false, false, 100.0f, 100.0f, 0.0, kConfig) ==
                  SelectionGestureAction::None,
              "a move with no press reports nothing");
    }

    if (failures != 0) {
        std::cerr << "android_selection: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_selection: ok\n";
    return 0;
}
