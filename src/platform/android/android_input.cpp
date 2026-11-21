// SPDX-License-Identifier: MIT
//
// NDK-facing Android input translation (issue #189). The mapping rules live in
// android_input_translation.cpp, which includes no NDK header and is unit-tested on every lane;
// this file only reads the event and calls them.

#include "android_input.hpp"
#include "android_input_translation.hpp"
#include "tessera/ui/key_code.hpp"

#include <android/input.h>

namespace tessera::hal {
namespace {

// The engine's monotonic seconds. Android's event time is nanoseconds since boot, which is the same
// clock `steady_now_seconds()` reads, so the timestamp is the event's own rather than "now": a
// coalesced or delayed event keeps the time it happened.
double seconds_from_nanos(std::int64_t nanos) {
    return static_cast<double>(nanos) / 1'000'000'000.0;
}

} // namespace

bool AndroidInputTranslator::pending() const {
    return queue_ != nullptr && !queue_->empty();
}

bool AndroidInputTranslator::translate(const AInputEvent* event) {
    if (event == nullptr || queue_ == nullptr) {
        return false;
    }
    switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_MOTION:
        return translate_motion(event);
    case AINPUT_EVENT_TYPE_KEY:
        return translate_key(event);
    default:
        return false;
    }
}

bool AndroidInputTranslator::translate_motion(const AInputEvent* event) {
    const std::int32_t action = AMotionEvent_getAction(event);
    const android::MotionAction mapped = android::motion_action(action);
    if (mapped.kind == android::MotionAction::Kind::Ignored) {
        return false;
    }

    const std::int32_t source = AInputEvent_getSource(event);
    const std::int32_t tool = AMotionEvent_getToolType(event, 0);
    const ui::PointerKind kind = android::pointer_kind_from(tool, source);
    const double timestamp = seconds_from_nanos(AMotionEvent_getEventTime(event));

    // A pointer action names its pointer by index; every other action uses index 0.
    const std::size_t index =
        mapped.pointer_index >= 0 ? static_cast<std::size_t>(mapped.pointer_index) : 0;
    const std::int32_t pointer_id = AMotionEvent_getPointerId(event, index);
    const float x = AMotionEvent_getX(event, index) / scale_;
    const float y = AMotionEvent_getY(event, index) / scale_;

    ui::InputEvent out;
    out.timestamp_s = timestamp;
    out.pointer_kind = kind;
    out.device_id = static_cast<std::uint32_t>(AInputEvent_getDeviceId(event));
    out.x = x;
    out.y = y;
    out.pressure = AMotionEvent_getPressure(event, index);

    switch (mapped.kind) {
    case android::MotionAction::Kind::Down:
        out.type = ui::InputEventType::PointerDown;
        out.button = ui::PointerButton::Left;
        out.is_primary = true;
        // The gesture's identity is fixed at DOWN: Android renumbers indices as fingers lift, so
        // the id is remembered here and reused for the rest of the gesture.
        active_pointer_id_ = pointer_id;
        gesture_active_ = true;
        break;
    case android::MotionAction::Kind::Up:
        out.type = ui::InputEventType::PointerUp;
        out.button = ui::PointerButton::Left;
        out.is_primary = true;
        gesture_active_ = false;
        break;
    case android::MotionAction::Kind::Move:
        out.type = ui::InputEventType::PointerMove;
        break;
    case android::MotionAction::Kind::Cancel:
        out.type = ui::InputEventType::PointerCancel;
        gesture_active_ = false;
        break;
    case android::MotionAction::Kind::HoverMove:
    case android::MotionAction::Kind::HoverEnter:
        // A hover is a move with no button: the router treats a move with no active press as hover.
        out.type = ui::InputEventType::PointerMove;
        break;
    case android::MotionAction::Kind::HoverExit:
        // Leaving the window is a cancel of any hover state, not a move to nowhere.
        out.type = ui::InputEventType::PointerCancel;
        break;
    case android::MotionAction::Kind::Scroll: {
        out.type = ui::InputEventType::Wheel;
        out.dx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, index);
        out.dy = android::scroll_dy_from_axis(
            AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, index));
        break;
    }
    case android::MotionAction::Kind::Ignored:
        return false;
    }

    // The pointer id the queue carries is the gesture's, not the event's index.
    out.pointer_id =
        static_cast<std::uint32_t>(active_pointer_id_ >= 0 ? active_pointer_id_ : pointer_id);
    queue_->push(out);
    return true;
}

bool AndroidInputTranslator::translate_key(const AInputEvent* event) {
    const std::int32_t action = AKeyEvent_getAction(event);
    if (action != android::kKeyActionDown && action != android::kKeyActionUp) {
        // A multiple-key event (a chord) is not a single key transition; the platform sends the
        // individual events too.
        return false;
    }

    const std::int32_t keycode = AKeyEvent_getKeyCode(event);
    const std::int32_t meta = AKeyEvent_getMetaState(event);
    const double timestamp = seconds_from_nanos(AKeyEvent_getEventTime(event));

    // The system Back request is a normalized event of its own (os-events-v1.md section 7), not a
    // key: the router routes it to the overlay stack and the navigation controller.
    if (keycode == android::kKeycodeBack) {
        if (action != android::kKeyActionDown) {
            return false;
        }
        ui::InputEvent out;
        out.timestamp_s = timestamp;
        out.type = ui::InputEventType::BackRequested;
        queue_->push(out);
        return true;
    }

    // The hardware volume keys are normalized too, so an app can react without a key binding.
    if (keycode == android::kKeycodeVolumeUp || keycode == android::kKeycodeVolumeDown) {
        if (action != android::kKeyActionDown) {
            return false;
        }
        ui::InputEvent out;
        out.timestamp_s = timestamp;
        out.type = ui::InputEventType::VolumeKey;
        out.volume_direction = keycode == android::kKeycodeVolumeUp ? ui::VolumeDirection::Up
                                                                    : ui::VolumeDirection::Down;
        queue_->push(out);
        return true;
    }

    const ui::KeyCode logical = android::logical_key_from_keycode(keycode);
    if (logical == ui::key::kUnknown) {
        // A key the engine has no code for: let the platform handle it rather than inventing one.
        return false;
    }

    ui::InputEvent out;
    out.timestamp_s = timestamp;
    out.type =
        action == android::kKeyActionDown ? ui::InputEventType::KeyDown : ui::InputEventType::KeyUp;
    out.key = logical;
    out.modifiers = android::modifiers_from_meta(meta);
    out.repeat = AKeyEvent_getRepeatCount(event) > 0;
    queue_->push(out);

    // Typed text: a key-down that produces a character also emits a Text event, which is what the
    // edit controller inserts. A key-up never types. The NDK has no unicode-char accessor, so the
    // character is derived from the keycode (see codepoint_from_key).
    if (action == android::kKeyActionDown) {
        const std::uint32_t codepoint = android::codepoint_from_key(keycode, out.modifiers);
        if (codepoint != 0) {
            ui::InputEvent text;
            text.timestamp_s = timestamp;
            text.type = ui::InputEventType::Text;
            text.text_codepoint = codepoint;
            text.modifiers = out.modifiers;
            queue_->push(text);
        }
    }
    return true;
}

} // namespace tessera::hal
