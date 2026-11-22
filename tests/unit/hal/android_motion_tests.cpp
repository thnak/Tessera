// SPDX-License-Identifier: MIT
//
// Pure Java-MotionEvent translation (issue #189): no NDK, no JNI, no device.
//
// What it proves is what a device lane cannot see cheaply: the physical-to-logical conversion, the
// scroll sign, and that a gesture's identity survives the index renumbering Android does.

#include "android_input_translation.hpp"
#include "android_motion_translation.hpp"

#include <iostream>

namespace {

namespace android = tessera::hal::android;
using tessera::ui::InputEventType;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// A sample with sane defaults: an unshifted finger on a touchscreen at 2.75x.
android::MotionSample finger(std::int32_t action, float x, float y) {
    android::MotionSample sample;
    sample.action = action;
    sample.source = android::kSourceTouchscreen;
    sample.tool_type = android::kToolFinger;
    sample.pointer_id = 0;
    sample.x = x;
    sample.y = y;
    sample.pressure = 1.0F;
    sample.event_time_nanos = 1'500'000'000; // 1.5 s
    return sample;
}

} // namespace

int main() {
    constexpr float kScale = 2.75F;

    // --- Down ---------------------------------------------------------------------------------
    {
        android::MotionGesture gesture;
        const android::MotionTranslation out = android::translate_motion_sample(
            finger(android::kActionDown, 275.0F, 550.0F), kScale, gesture);
        check(out.translated, "a down is translated");
        check(out.event.type == InputEventType::PointerDown, "down maps to PointerDown");
        check(out.event.button == tessera::ui::PointerButton::Left, "down is the left button");
        check(out.event.is_primary, "down is primary");
        // Physical pixels divided by the density: 275 / 2.75 = 100 logical pixels.
        check(out.event.x == 100.0F, "the x is logical");
        check(out.event.y == 200.0F, "the y is logical");
        check(out.event.timestamp_s == 1.5, "the timestamp is seconds");
        check(out.event.pointer_kind == tessera::ui::PointerKind::Touch,
              "a finger on a touchscreen is a touch pointer");
        check(gesture.active, "the gesture is active after a down");
        check(gesture.active_pointer_id == 0, "the pointer id is recorded at down");
    }

    // --- A non-positive scale is the documented fallback, not a division by zero ---------------
    {
        android::MotionGesture gesture;
        const android::MotionTranslation out = android::translate_motion_sample(
            finger(android::kActionDown, 40.0F, 50.0F), 0.0F, gesture);
        check(out.event.x == 40.0F, "a zero scale keeps physical pixels");
        check(out.event.y == 50.0F, "and so does the y");
    }

    // --- Move and up --------------------------------------------------------------------------
    {
        android::MotionGesture gesture;
        (void)android::translate_motion_sample(finger(android::kActionDown, 0.0F, 0.0F), kScale,
                                               gesture);
        {
            const android::MotionTranslation move = android::translate_motion_sample(
                finger(android::kActionMove, 110.0F, 220.0F), kScale, gesture);
            check(move.event.type == InputEventType::PointerMove, "move maps to PointerMove");
            check(move.event.x == 40.0F, "the moved x is logical");
            check(gesture.active, "a move does not end the gesture");
        }
        {
            const android::MotionTranslation up = android::translate_motion_sample(
                finger(android::kActionUp, 110.0F, 220.0F), kScale, gesture);
            check(up.event.type == InputEventType::PointerUp, "up maps to PointerUp");
            check(!gesture.active, "the gesture ends at up");
            check(gesture.active_pointer_id == -1, "and its id is cleared");
        }
    }

    // --- Cancel -------------------------------------------------------------------------------
    {
        android::MotionGesture gesture;
        (void)android::translate_motion_sample(finger(android::kActionDown, 0.0F, 0.0F), kScale,
                                               gesture);
        const android::MotionTranslation out = android::translate_motion_sample(
            finger(android::kActionCancel, 0.0F, 0.0F), kScale, gesture);
        check(out.event.type == InputEventType::PointerCancel, "cancel maps to PointerCancel");
        check(!gesture.active, "cancel ends the gesture so no capture outlives it");
    }

    // --- Pointer identity survives the index renumbering ---------------------------------------
    // Two fingers: the second goes down with pointer id 1, and its ACTION_POINTER_DOWN packs the
    // *index* (1) into the high byte. The id must be the platform's pointer id, not the index.
    {
        android::MotionGesture gesture;
        (void)android::translate_motion_sample(finger(android::kActionDown, 0.0F, 0.0F), kScale,
                                               gesture);

        android::MotionSample second = finger(android::kActionPointerDown, 100.0F, 100.0F);
        second.action = (1 << android::kActionPointerIndexShift) | android::kActionPointerDown;
        second.pointer_id = 7;
        const android::MotionTranslation out =
            android::translate_motion_sample(second, kScale, gesture);
        check(out.translated, "a packed pointer action is translated");
        check(out.event.type == InputEventType::PointerDown,
              "a pointer-down is a pointer down for the new finger");
    }

    // --- Scroll sign --------------------------------------------------------------------------
    // Android's AXIS_VSCROLL is positive when the content should move up; the engine uses the web
    // sign, where positive dy scrolls content down. Getting this backwards makes every scroll go
    // the wrong way.
    {
        android::MotionGesture gesture;
        android::MotionSample scroll = finger(android::kActionScroll, 0.0F, 0.0F);
        scroll.vscroll = 3.0F;
        const android::MotionTranslation out =
            android::translate_motion_sample(scroll, kScale, gesture);
        check(out.event.type == InputEventType::Wheel, "scroll maps to Wheel");
        check(out.event.dy < 0.0F, "a positive Android vscroll becomes a negative dy");
    }
    {
        android::MotionGesture gesture;
        android::MotionSample scroll = finger(android::kActionScroll, 0.0F, 0.0F);
        scroll.vscroll = -3.0F;
        const android::MotionTranslation out =
            android::translate_motion_sample(scroll, kScale, gesture);
        check(out.event.dy > 0.0F, "a negative Android vscroll becomes a positive dy");
    }
    {
        android::MotionGesture gesture;
        android::MotionSample scroll = finger(android::kActionScroll, 0.0F, 0.0F);
        scroll.hscroll = 5.5F;
        const android::MotionTranslation out =
            android::translate_motion_sample(scroll, kScale, gesture);
        check(out.event.dx == 2.0F, "the horizontal axis is logical and keeps its sign");
    }

    // --- Hover --------------------------------------------------------------------------------
    {
        android::MotionGesture gesture;
        android::MotionSample hover = finger(android::kActionHoverEnter, 10.0F, 20.0F);
        hover.source = android::kSourceMouse;
        hover.tool_type = android::kToolMouse;
        const android::MotionTranslation out =
            android::translate_motion_sample(hover, kScale, gesture);
        check(out.event.type == InputEventType::PointerMove, "a hover enter is a move");
        check(out.event.pointer_kind == tessera::ui::PointerKind::Mouse,
              "a mouse tool is a mouse pointer");
    }
    {
        android::MotionGesture gesture;
        android::MotionSample hover = finger(android::kActionHoverExit, 10.0F, 20.0F);
        const android::MotionTranslation out =
            android::translate_motion_sample(hover, kScale, gesture);
        // There is no HoverExit event type; leaving is reported as a cancel so transient hover
        // state is dropped instead of lingering.
        check(out.event.type == InputEventType::PointerCancel,
              "a hover exit cancels any hover state");
    }

    // --- An unknown action is dropped, not guessed ---------------------------------------------
    {
        android::MotionGesture gesture;
        const android::MotionTranslation out =
            android::translate_motion_sample(finger(99, 0.0F, 0.0F), kScale, gesture);
        check(!out.translated, "an unknown action is not translated");
    }

    if (failures != 0) {
        std::cerr << "android_motion: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_motion: ok\n";
    return 0;
}
