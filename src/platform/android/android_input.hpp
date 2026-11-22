// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

struct AInputEvent;

namespace tessera::hal {

// Translates Android `AInputEvent`s into `ui::InputEvent` and pushes them into the application's
// queue (issue #189, docs/design/mobile-integration.md section 3.2).
//
// Everything runs on the thread that owns the window: `android_native_app_glue` delivers input on
// its own looper thread, which is the same thread `android_main` runs on, so the queue's single
// producer is that thread. The translator never allocates and never blocks.
//
// Coordinates: Android reports physical pixels; the queue carries logical pixels, so every position
// is divided by the display scale. The scale is the one the window was created with, so a density
// change re-creates the window and the translator with it.
//
// What is deliberately not translated yet:
//   - Historical/coalesced samples (`AMotionEvent_getHistorical*`). The spec asks for them
//     (section 2, "historical/coalesced samples"); they need a bounded batch API on the queue,
//     which is its own slice. Today the newest sample is used, which loses sub-frame motion but
//     never loses an event.
//   - Pressure, tilt and orientation. The queue has `pressure`; tilt/orientation have no field yet.
//   - IME composition (`InputConnection`), which is section 3.3.
class AndroidInputTranslator {
public:
    void set_queue(ui::InputQueue* queue) {
        queue_ = queue;
    }

    // The display scale the window was created with (physical / logical pixels).
    void set_scale(float scale) {
        scale_ = scale > 0.0F ? scale : 1.0F;
    }

    // True while the queue holds input the application has not consumed yet; a wait uses this to
    // wake for input, since input never becomes a WindowEvent.
    [[nodiscard]] bool pending() const;

    // Translates one event and returns true when it produced at least one queued event. A false
    // return means the event was not input the engine uses (a key it has no code for, a motion
    // action it ignores), and the caller should let the platform handle it.
    bool translate(const AInputEvent* event);

    // Queues an event the HOST already translated, for the Java MotionEvent path: the shell is a
    // Java activity, so the platform reports touch as a Java object and the host maps it with the
    // pure rules in android_motion_translation.cpp. Returns false when no queue is installed.
    bool push(const ui::InputEvent& event);

private:
    bool translate_motion(const AInputEvent* event);
    bool translate_key(const AInputEvent* event);

    ui::InputQueue* queue_ = nullptr;
    float scale_ = 1.0F;
    // The pointer id the current gesture started with. Android reports a pointer index per event,
    // which is not stable across a gesture (a lifted finger renumbers the rest), so the id is
    // resolved once at DOWN and reused until the gesture ends.
    std::int32_t active_pointer_id_ = -1;
    bool gesture_active_ = false;
};

} // namespace tessera::hal
