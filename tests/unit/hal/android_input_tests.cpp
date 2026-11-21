// SPDX-License-Identifier: MIT
//
// Android input mapping rules (issue #189, docs/design/mobile-integration.md section 3.2).
//
// The rules are pure over stable ABI constants, so they run on every lane without an NDK or a
// device. What they prove is exactly what a device lane cannot see cheaply: the sign of the scroll
// axis, the pointer-index packing, the cancel semantics, and that an unmapped key is a no-op rather
// than a wrong action.

#include "android_input_translation.hpp"
#include "tessera/ui/key_code.hpp"

#include <cstdint>
#include <iostream>

namespace {

namespace android = tessera::hal::android;
namespace key = tessera::ui::key;
using tessera::ui::Modifier;
using tessera::ui::PointerKind;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // --- Modifiers ---------------------------------------------------------------------------
    check(android::modifiers_from_meta(0) == Modifier::None, "no meta bits is no modifier");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaShiftOn),
                                    Modifier::Shift),
          "shift bit maps to Shift");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaCtrlOn),
                                    Modifier::Control),
          "ctrl bit maps to Control");
    check(
        tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaAltOn), Modifier::Alt),
        "alt bit maps to Alt");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaMetaOn),
                                    Modifier::Meta),
          "meta bit maps to Meta");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaCapsLockOn),
                                    Modifier::CapsLock),
          "caps-lock bit maps to CapsLock");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaNumLockOn),
                                    Modifier::NumLock),
          "num-lock bit maps to NumLock");
    // Combined bits accumulate rather than overwrite.
    const Modifier both =
        android::modifiers_from_meta(android::kMetaShiftOn | android::kMetaCtrlOn);
    check(tessera::ui::has_modifier(both, Modifier::Shift) &&
              tessera::ui::has_modifier(both, Modifier::Control),
          "shift and ctrl combine");

    // --- Pointer kind ------------------------------------------------------------------------
    check(android::pointer_kind_from(android::kToolFinger, android::kSourceTouchscreen) ==
              PointerKind::Touch,
          "finger is touch");
    check(android::pointer_kind_from(android::kToolStylus, android::kSourceStylus) ==
              PointerKind::Pen,
          "stylus is pen");
    check(android::pointer_kind_from(android::kToolEraser, android::kSourceStylus) ==
              PointerKind::Pen,
          "eraser is pen");
    check(android::pointer_kind_from(android::kToolMouse, android::kSourceMouse) ==
              PointerKind::Mouse,
          "mouse is mouse");
    // An unknown tool type falls back to the source, not to a guess.
    check(android::pointer_kind_from(0, android::kSourceMouse) == PointerKind::Mouse,
          "unknown tool with a mouse source is a mouse");
    check(android::pointer_kind_from(0, android::kSourceStylus) == PointerKind::Pen,
          "unknown tool with a stylus source is a pen");
    check(android::pointer_kind_from(0, android::kSourceTouchscreen) == PointerKind::Touch,
          "unknown tool with a touchscreen source is touch");
    // A completely unknown source is touch: it cannot hover, which is the safe answer.
    check(android::pointer_kind_from(0, 0) == PointerKind::Touch,
          "unknown source defaults to touch");

    // --- Logical keys ------------------------------------------------------------------------
    check(android::logical_key_from_keycode(android::kKeycodeEnter) == key::kEnter, "enter");
    check(android::logical_key_from_keycode(android::kKeycodeTab) == key::kTab, "tab");
    check(android::logical_key_from_keycode(android::kKeycodeSpace) == key::kSpace, "space");
    check(android::logical_key_from_keycode(android::kKeycodeDel) == key::kBackspace,
          "del is backspace");
    check(android::logical_key_from_keycode(android::kKeycodeForwardDel) == key::kBackspace,
          "forward-del is backspace");
    check(android::logical_key_from_keycode(android::kKeycodeEscape) == key::kEscape, "escape");
    check(android::logical_key_from_keycode(android::kKeycodeDpadLeft) == key::kArrowLeft,
          "dpad-left is arrow-left");
    check(android::logical_key_from_keycode(android::kKeycodeDpadUp) == key::kArrowUp,
          "dpad-up is arrow-up");
    check(android::logical_key_from_keycode(android::kKeycodeDpadRight) == key::kArrowRight,
          "dpad-right is arrow-right");
    check(android::logical_key_from_keycode(android::kKeycodeDpadDown) == key::kArrowDown,
          "dpad-down is arrow-down");
    check(android::logical_key_from_keycode(android::kKeycodeMoveHome) == key::kHome, "home");
    check(android::logical_key_from_keycode(android::kKeycodeMoveEnd) == key::kEnd, "end");
    check(android::logical_key_from_keycode(android::kKeycodePageUp) == key::kPageUp, "page-up");
    check(android::logical_key_from_keycode(android::kKeycodePageDown) == key::kPageDown,
          "page-down");
    check(android::logical_key_from_keycode(android::kKeycodeShiftLeft) == key::kShift,
          "shift-left");
    check(android::logical_key_from_keycode(android::kKeycodeShiftRight) == key::kShift,
          "shift-right");
    check(android::logical_key_from_keycode(android::kKeycodeCtrlLeft) == key::kControl,
          "ctrl-left");
    check(android::logical_key_from_keycode(android::kKeycodeAltLeft) == key::kAlt, "alt-left");
    check(android::logical_key_from_keycode(android::kKeycodeMetaLeft) == key::kMeta, "meta-left");
    check(android::logical_key_from_keycode(android::kKeycodeCapsLock) == key::kCapsLock,
          "caps-lock");

    // Letters and digits are contiguous ranges, and they map to upper-case ASCII.
    check(android::logical_key_from_keycode(android::kKeycodeA) == 'A', "A maps to 'A'");
    check(android::logical_key_from_keycode(android::kKeycodeZ) == 'Z', "Z maps to 'Z'");
    check(android::logical_key_from_keycode(android::kKeycode0) == '0', "0 maps to '0'");
    check(android::logical_key_from_keycode(android::kKeycode9) == '9', "9 maps to '9'");
    check(android::logical_key_from_keycode(android::kKeycodeF1) == key::kF1, "F1");
    check(android::logical_key_from_keycode(android::kKeycodeF12) == key::kF1 + 11, "F12");

    // An unmapped key is a no-op, never a wrong action.
    check(android::logical_key_from_keycode(0) == key::kUnknown, "keycode 0 is unknown");
    check(android::logical_key_from_keycode(9999) == key::kUnknown,
          "an unmapped keycode is unknown");

    // --- Typed text --------------------------------------------------------------------------
    // The NDK has no unicode-char accessor, so the character is derived from the keycode. Letters
    // and digits are the only unambiguous cases; everything else comes from the IME bridge.
    check(android::codepoint_from_key(android::kKeycodeA, Modifier::None) == 'a',
          "unshifted A types 'a'");
    check(tessera::ui::has_modifier(android::modifiers_from_meta(android::kMetaShiftOn),
                                    Modifier::Shift),
          "shift is available for the case test");
    check(android::codepoint_from_key(android::kKeycodeA, Modifier::Shift) == 'A',
          "shifted A types 'A'");
    check(android::codepoint_from_key(android::kKeycodeZ, Modifier::None) == 'z',
          "unshifted Z types 'z'");
    check(android::codepoint_from_key(android::kKeycode0, Modifier::None) == '0',
          "unshifted 0 types '0'");
    check(android::codepoint_from_key(android::kKeycode9, Modifier::None) == '9',
          "unshifted 9 types '9'");
    check(android::codepoint_from_key(android::kKeycodeSpace, Modifier::None) == ' ',
          "space types a space");
    // A shifted digit is a layout-dependent symbol (!@#$...), so it is left to the IME bridge
    // rather than guessed wrong.
    check(android::codepoint_from_key(android::kKeycode0, Modifier::Shift) == 0,
          "a shifted digit types nothing here");
    // A non-typing key types nothing.
    check(android::codepoint_from_key(android::kKeycodeEnter, Modifier::None) == 0,
          "enter types nothing");
    check(android::codepoint_from_key(android::kKeycodeDpadLeft, Modifier::None) == 0,
          "an arrow types nothing");
    check(android::codepoint_from_key(9999, Modifier::None) == 0,
          "an unmapped keycode types nothing");

    // --- Motion actions ----------------------------------------------------------------------
    check(android::motion_action(android::kActionDown).kind == android::MotionAction::Kind::Down,
          "ACTION_DOWN is Down");
    check(android::motion_action(android::kActionUp).kind == android::MotionAction::Kind::Up,
          "ACTION_UP is Up");
    check(android::motion_action(android::kActionMove).kind == android::MotionAction::Kind::Move,
          "ACTION_MOVE is Move");
    check(android::motion_action(android::kActionCancel).kind ==
              android::MotionAction::Kind::Cancel,
          "ACTION_CANCEL is Cancel");
    // ACTION_OUTSIDE ends the gesture: the engine must not keep the capture.
    check(android::motion_action(android::kActionOutside).kind ==
              android::MotionAction::Kind::Cancel,
          "ACTION_OUTSIDE is Cancel");
    check(android::motion_action(android::kActionHoverMove).kind ==
              android::MotionAction::Kind::HoverMove,
          "ACTION_HOVER_MOVE is HoverMove");
    check(android::motion_action(android::kActionScroll).kind ==
              android::MotionAction::Kind::Scroll,
          "ACTION_SCROLL is Scroll");

    // The pointer index is packed into the high byte of the pointer actions.
    const std::int32_t pointer_down = android::kActionPointerDown | (2 << 8);
    const auto pointer = android::motion_action(pointer_down);
    check(pointer.kind == android::MotionAction::Kind::Down, "POINTER_DOWN is Down");
    check(pointer.pointer_index == 2, "POINTER_DOWN carries pointer index 2");
    const std::int32_t pointer_up = android::kActionPointerUp | (1 << 8);
    const auto lifted = android::motion_action(pointer_up);
    check(lifted.kind == android::MotionAction::Kind::Up, "POINTER_UP is Up");
    check(lifted.pointer_index == 1, "POINTER_UP carries pointer index 1");
    // A non-pointer action carries no index.
    check(android::motion_action(android::kActionMove).pointer_index == -1,
          "ACTION_MOVE carries no pointer index");

    // --- Cancel semantics --------------------------------------------------------------------
    check(android::action_cancels_gesture(android::kActionCancel), "CANCEL cancels the gesture");
    check(android::action_cancels_gesture(android::kActionOutside), "OUTSIDE cancels the gesture");
    check(!android::action_cancels_gesture(android::kActionUp), "UP does not cancel");
    check(!android::action_cancels_gesture(android::kActionMove), "MOVE does not cancel");

    // --- Scroll sign -------------------------------------------------------------------------
    // Android's AXIS_VSCROLL is positive when the content should move up; the engine uses the web
    // sign, where positive dy scrolls content down. Getting this backwards makes every wheel and
    // two-finger scroll go the wrong way, which is exactly the defect a unit test should catch.
    check(android::scroll_dy_from_axis(1.0F) == -1.0F, "a positive Android axis is negative dy");
    check(android::scroll_dy_from_axis(-1.0F) == 1.0F, "a negative Android axis is positive dy");
    check(android::scroll_dy_from_axis(0.0F) == 0.0F, "a zero axis is zero dy");

    if (failures != 0) {
        std::cerr << "android_input: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_input: ok\n";
    return 0;
}
