// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

// Pure translation of one Java `MotionEvent` sample (issue #189, docs/design/mobile-integration.md
// section 3.2).
//
// The shell is a Java activity (see `docs/guides/android-lane.md` section 3.6), so touch arrives as
// a `android.view.MotionEvent` rather than as the NDK's `AInputEvent`. The JNI bridge cannot read a
// `MotionEvent` directly — it is a Java object with getters — so it reads the scalar fields once
// and fills `MotionSample`, and THIS header does the mapping.
//
// Same split as the NDK path (`android_input.cpp` + `android_input_translation.cpp`), and for the
// same reason: the rules are plain numbers, so they compile and run on every lane with no NDK, no
// device and no JNI. A defect in the scroll sign or the pointer bookkeeping is caught by a unit
// test instead of by a finger on a phone.
//
// The action, tool and source constants are the ones in `android_input_translation.hpp`; this
// header reuses them rather than restating them.

namespace tessera::hal::android {

// One MotionEvent, reduced to the scalars the mapping needs.
//
// A `MotionEvent` carries every pointer in the gesture; this is the ONE sample the mapping converts
// this call, already resolved to the pointer the action names. The bridge does that resolution
// because it needs the Java getters anyway, and keeping it there means this struct is trivially
// constructible in a test.
struct MotionSample {
    // `MotionEvent.getAction()`: the low byte is the action, the high byte the pointer index for
    // the pointer actions. Exactly what `AMotionEvent_getAction` returns.
    std::int32_t action = 0;
    // `MotionEvent.getEventTime()`, nanoseconds.
    std::int64_t event_time_nanos = 0;
    // `MotionEvent.getDeviceId()`.
    std::int32_t device_id = 0;
    // `MotionEvent.getSource()`.
    std::int32_t source = 0;
    // `MotionEvent.getToolType(index)`.
    std::int32_t tool_type = 0;
    // `MotionEvent.getPointerId(index)`.
    std::int32_t pointer_id = 0;
    // `MotionEvent.getX(index)` / `getY(index)`, in PHYSICAL pixels.
    float x = 0.0F;
    float y = 0.0F;
    // `MotionEvent.getPressure(index)`.
    float pressure = 0.0F;
    // `MotionEvent.getAxisValue(MotionEvent.AXIS_VSCROLL)`.
    float vscroll = 0.0F;
    // `MotionEvent.getAxisValue(MotionEvent.AXIS_HSCROLL)`.
    float hscroll = 0.0F;
};

// The gesture bookkeeping the Java path needs, mirroring `AndroidInputTranslator`'s members: the
// pointer id is fixed at DOWN because Android renumbers indices as fingers lift.
struct MotionGesture {
    std::int32_t active_pointer_id = -1;
    bool active = false;

    // Forgets the gesture. Called on UP and on CANCEL so a stale capture cannot outlive it.
    void reset() noexcept {
        active_pointer_id = -1;
        active = false;
    }
};

// What one sample means for the normalized queue. `translated` is false when the bus should drop
// it. The flag is declared AFTER `event` deliberately: putting a `bool` first pads the struct by
// seven bytes to reach the event's alignment, which MSVC reports as C4324 and the build treats as
// an error.
struct MotionTranslation {
    ui::InputEvent event;
    bool translated = false;
};

// Converts one sample. `scale` is the display density: the platform reports physical pixels and the
// engine works in logical ones, so every coordinate is divided by it. A non-positive scale is
// treated as 1.0 (the documented "unknown density" fallback) rather than dividing by zero.
//
// `gesture` is updated in place: Down records the pointer id and the x/y where the gesture began,
// Up and Cancel reset it. That is the whole state the translation needs, and keeping it here rather
// than in the bridge is what lets a test drive a full press-drag-release sequence.
[[nodiscard]] MotionTranslation translate_motion_sample(const MotionSample& sample, float scale,
                                                        MotionGesture& gesture) noexcept;

} // namespace tessera::hal::android
