// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

// Pure mapping rules for Android input (issue #189, docs/design/mobile-integration.md section 3.2).
//
// This header deliberately does not include <android/input.h>: the action, tool-type, source and
// keycode constants are stable ABI values, so the rules are unit-testable on any platform without
// an NDK or a device. The NDK-facing translator (android_input.cpp) calls into these.
//
// The constants below are the values from <android/input.h> and <android/keycodes.h>. They are
// restated rather than included for the same reason the X11 rules restate the core-protocol masks:
// a test lane with no NDK must still be able to compile and exercise the mapping.

namespace tessera::hal::android {

// --- MotionEvent actions (AMOTION_EVENT_ACTION_*) ---------------------------------------------
inline constexpr std::int32_t kActionDown = 0;
inline constexpr std::int32_t kActionUp = 1;
inline constexpr std::int32_t kActionMove = 2;
inline constexpr std::int32_t kActionCancel = 3;
inline constexpr std::int32_t kActionOutside = 4;
inline constexpr std::int32_t kActionPointerDown = 5;
inline constexpr std::int32_t kActionPointerUp = 6;
inline constexpr std::int32_t kActionHoverMove = 7;
inline constexpr std::int32_t kActionHoverEnter = 9;
inline constexpr std::int32_t kActionHoverExit = 10;
inline constexpr std::int32_t kActionScroll = 8;
// The action is the low byte; the pointer index is packed into the high byte for the pointer
// actions. `kActionPointerIndexShift` is the shift the platform uses.
inline constexpr std::int32_t kActionPointerIndexShift = 8;
inline constexpr std::int32_t kActionMask = 0xFF;

// --- Tool types (AMOTION_EVENT_TOOL_TYPE_*) ---------------------------------------------------
inline constexpr std::int32_t kToolFinger = 1;
inline constexpr std::int32_t kToolStylus = 2;
inline constexpr std::int32_t kToolMouse = 3;
inline constexpr std::int32_t kToolEraser = 4;

// --- Input sources (AINPUT_SOURCE_*) ----------------------------------------------------------
inline constexpr std::int32_t kSourceTouchscreen = 0x00001002;
inline constexpr std::int32_t kSourceMouse = 0x00002002;
inline constexpr std::int32_t kSourceStylus = 0x00004002;
inline constexpr std::int32_t kSourceTrackball = 0x00010004;
inline constexpr std::int32_t kSourceTouchpad = 0x00100008;
inline constexpr std::int32_t kSourceClassMask = 0x0000000F;
inline constexpr std::int32_t kSourceClassButton = 0x00000001;

// --- KeyEvent actions (AKEY_EVENT_ACTION_*) ---------------------------------------------------
inline constexpr std::int32_t kKeyActionDown = 0;
inline constexpr std::int32_t kKeyActionUp = 1;
inline constexpr std::int32_t kKeyActionMultiple = 2;

// --- Meta state bits (AMETA_*) ----------------------------------------------------------------
inline constexpr std::int32_t kMetaShiftOn = 0x1;
inline constexpr std::int32_t kMetaAltOn = 0x2;
inline constexpr std::int32_t kMetaSymOn = 0x4;
inline constexpr std::int32_t kMetaCtrlOn = 0x1000;
inline constexpr std::int32_t kMetaMetaOn = 0x10000;
inline constexpr std::int32_t kMetaCapsLockOn = 0x100000;
inline constexpr std::int32_t kMetaNumLockOn = 0x2000000;

// --- Keycodes (AKEYCODE_*) --------------------------------------------------------------------
// Only the keys the engine has a logical code for. Everything else maps to `ui::key::kUnknown`,
// which the router ignores, so an unmapped key is a no-op rather than a wrong action.
inline constexpr std::int32_t kKeycodeBack = 4;
inline constexpr std::int32_t kKeycodeEnter = 66;
inline constexpr std::int32_t kKeycodeTab = 61;
inline constexpr std::int32_t kKeycodeSpace = 62;
inline constexpr std::int32_t kKeycodeDel = 67;
inline constexpr std::int32_t kKeycodeForwardDel = 112;
inline constexpr std::int32_t kKeycodeEscape = 111;
inline constexpr std::int32_t kKeycodeDpadLeft = 21;
inline constexpr std::int32_t kKeycodeDpadUp = 19;
inline constexpr std::int32_t kKeycodeDpadRight = 22;
inline constexpr std::int32_t kKeycodeDpadDown = 20;
inline constexpr std::int32_t kKeycodeMoveHome = 122;
inline constexpr std::int32_t kKeycodeMoveEnd = 123;
inline constexpr std::int32_t kKeycodePageUp = 92;
inline constexpr std::int32_t kKeycodePageDown = 93;
inline constexpr std::int32_t kKeycodeInsert = 124;
inline constexpr std::int32_t kKeycodeShiftLeft = 59;
inline constexpr std::int32_t kKeycodeShiftRight = 60;
inline constexpr std::int32_t kKeycodeCtrlLeft = 113;
inline constexpr std::int32_t kKeycodeCtrlRight = 114;
inline constexpr std::int32_t kKeycodeAltLeft = 57;
inline constexpr std::int32_t kKeycodeAltRight = 58;
inline constexpr std::int32_t kKeycodeMetaLeft = 117;
inline constexpr std::int32_t kKeycodeMetaRight = 118;
inline constexpr std::int32_t kKeycodeCapsLock = 115;
inline constexpr std::int32_t kKeycodeVolumeUp = 24;
inline constexpr std::int32_t kKeycodeVolumeDown = 25;
inline constexpr std::int32_t kKeycodeA = 29;
inline constexpr std::int32_t kKeycodeZ = 54;
inline constexpr std::int32_t kKeycode0 = 7;
inline constexpr std::int32_t kKeycode9 = 16;
inline constexpr std::int32_t kKeycodeF1 = 131;
inline constexpr std::int32_t kKeycodeF12 = 142;

// The normalized modifier state for an Android meta state.
[[nodiscard]] ui::Modifier modifiers_from_meta(std::int32_t meta_state);

// The physical device kind for an Android tool type, falling back to the input source when the
// tool type is unknown (a device that reports no tool type still reports a source).
[[nodiscard]] ui::PointerKind pointer_kind_from(std::int32_t tool_type, std::int32_t source);

// The logical key for an Android keycode, or `ui::key::kUnknown` when the engine has no code for
// it. Letters and digits map to upper-case ASCII, matching `ui/key_code.hpp`.
[[nodiscard]] ui::KeyCode logical_key_from_keycode(std::int32_t keycode);

// The Unicode scalar a key event types, or 0 when it types nothing.
//
// The NDK exposes no `AKeyEvent_getUnicodeChar` (only the Java `KeyEvent` has one), so the
// character is derived from the keycode and the modifier state. That covers the letters, digits and
// the punctuation the engine has keycodes for; anything else types nothing and the app receives
// only the key event. This is the documented limitation of the native input path: a full text path
// needs the IME bridge (`InputConnection`, section 3.3), which is where non-ASCII and composed text
// come from on Android anyway.
[[nodiscard]] std::uint32_t codepoint_from_key(std::int32_t keycode, ui::Modifier modifiers);

// One Java `KeyEvent`, reduced to the values the mapping needs. The shell is a Java activity, so a
// hardware key and an IME-synthesised key both arrive as a `KeyEvent` rather than as the NDK's
// `AInputEvent`; this is the Java-side twin of the `AKeyEvent` path in `android_input.cpp`.
struct KeySample {
    std::int32_t keycode = 0;
    std::int32_t meta_state = 0;
    // `kKeyActionDown` or `kKeyActionUp`; anything else (a chord's multiple-action event) is
    // ignored, exactly as the NDK path ignores it.
    std::int32_t action = kKeyActionDown;
    std::int32_t repeat_count = 0;
    double timestamp_s = 0.0;
};

// What one key event means for the queue: at most two events, because a key-down that types a
// character produces the key event AND a `Text` event (the edit controller inserts the character
// from the latter). Bounded and allocation-free, so it is safe on the platform's callback thread.
struct KeyTranslation {
    static constexpr std::size_t kMaxEvents = 2;
    ui::InputEvent events[kMaxEvents]{};
    std::size_t count = 0;
};

// Converts one key event. A key the engine has no logical code for produces nothing, so the caller
// can let the platform handle it instead of swallowing it. The system Back key is deliberately NOT
// translated here: the Java shell leaves it to the platform (see `TesseraView.dispatchKeyEvent`),
// because the engine's `BackRequested` needs a route stack the shell does not have.
[[nodiscard]] KeyTranslation translate_key_sample(const KeySample& sample) noexcept;

// What one MotionEvent action means for the normalized queue. `pointer_index` is the index the
// action's high byte carried, or -1 when the action is not a pointer action.
struct MotionAction {
    enum class Kind : std::uint8_t {
        Ignored,
        Down,
        Up,
        Move,
        Cancel,
        HoverMove,
        HoverEnter,
        HoverExit,
        Scroll,
    };
    Kind kind = Kind::Ignored;
    std::int32_t pointer_index = -1;
};

[[nodiscard]] MotionAction motion_action(std::int32_t action);

// True when the action ends a gesture, so the translator must release any capture it holds. Android
// sends ACTION_CANCEL when the system takes the gesture (a back gesture, a notification shade
// pull), and the engine must not keep a stale active state.
[[nodiscard]] bool action_cancels_gesture(std::int32_t action);

// The wheel delta for one ACTION_SCROLL event, in the web sign the engine uses (positive dy scrolls
// content down). Android reports the opposite sign in `AXIS_VSCROLL`, so the vertical axis is
// negated; the horizontal axis already matches.
[[nodiscard]] float scroll_dy_from_axis(float axis_vscroll);

} // namespace tessera::hal::android
