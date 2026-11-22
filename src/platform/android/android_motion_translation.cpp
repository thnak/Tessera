// SPDX-License-Identifier: MIT
//
// Pure translation of one Java MotionEvent sample (issue #189). No NDK, no JNI, so it runs on every
// lane; the JNI bridge reads the Java getters and calls this.

#include "android_input_translation.hpp"
#include "android_motion_translation.hpp"

#include <cmath>

namespace tessera::hal::android {
namespace {

// Nanoseconds to seconds. A double seconds value is what `ui::InputEvent` carries, and it is the
// same conversion the NDK path does, so a Java event and a native event land on the same timeline.
double seconds_from_nanos(std::int64_t nanos) noexcept {
    return static_cast<double>(nanos) / 1'000'000'000.0;
}

} // namespace

MotionTranslation translate_motion_sample(const MotionSample& sample, float scale,
                                          MotionGesture& gesture) noexcept {
    MotionTranslation result;
    const MotionAction mapped = motion_action(sample.action);
    if (mapped.kind == MotionAction::Kind::Ignored) {
        return result;
    }

    // The platform reports physical pixels and the engine works in logical ones. A non-positive
    // scale is the documented "unknown density" fallback rather than a division by zero.
    const float divisor = scale > 0.0F ? scale : 1.0F;

    ui::InputEvent out;
    out.timestamp_s = seconds_from_nanos(sample.event_time_nanos);
    out.pointer_kind = pointer_kind_from(sample.tool_type, sample.source);
    out.device_id = static_cast<std::uint32_t>(sample.device_id);
    out.x = sample.x / divisor;
    out.y = sample.y / divisor;
    out.pressure = sample.pressure;

    switch (mapped.kind) {
    case MotionAction::Kind::Down:
        out.type = ui::InputEventType::PointerDown;
        out.button = ui::PointerButton::Left;
        out.is_primary = true;
        // The gesture's identity is fixed at DOWN: Android renumbers indices as fingers lift, so
        // the id is remembered here and reused for the rest of the gesture.
        gesture.active_pointer_id = sample.pointer_id;
        gesture.active = true;
        break;
    case MotionAction::Kind::Up:
        out.type = ui::InputEventType::PointerUp;
        out.button = ui::PointerButton::Left;
        out.is_primary = true;
        gesture.reset();
        break;
    case MotionAction::Kind::Move:
        out.type = ui::InputEventType::PointerMove;
        break;
    case MotionAction::Kind::Cancel:
        // The system took the gesture (a back gesture, the notification shade). The engine must
        // release its capture rather than keep a stale active state.
        out.type = ui::InputEventType::PointerCancel;
        gesture.reset();
        break;
    case MotionAction::Kind::HoverMove:
    case MotionAction::Kind::HoverEnter:
        // A hover is a move with no button: the router treats a move with no active press as hover.
        out.type = ui::InputEventType::PointerMove;
        break;
    case MotionAction::Kind::HoverExit:
        // There is no HoverExit event type; a hover that leaves is the end of the pointer's
        // presence and is reported as a cancel so any transient hover state is dropped.
        out.type = ui::InputEventType::PointerCancel;
        break;
    case MotionAction::Kind::Scroll:
        out.type = ui::InputEventType::Wheel;
        // Android's AXIS_VSCROLL is positive when the content should move up; the engine uses the
        // web sign, where positive dy scrolls content down. The horizontal axis already matches.
        out.dy = scroll_dy_from_axis(sample.vscroll);
        out.dx = sample.hscroll / divisor;
        break;
    case MotionAction::Kind::Ignored:
        return result;
    }

    result.translated = true;
    result.event = out;
    return result;
}

} // namespace tessera::hal::android
