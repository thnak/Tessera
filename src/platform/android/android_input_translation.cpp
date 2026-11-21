// SPDX-License-Identifier: MIT
//
// Pure mapping rules for Android input (issue #189). No NDK header is included, so this compiles
// and runs on every lane; the NDK-facing translator calls into it.

#include "android_input_translation.hpp"
#include "tessera/ui/key_code.hpp"

namespace tessera::hal::android {

ui::Modifier modifiers_from_meta(std::int32_t meta_state) {
    ui::Modifier modifiers = ui::Modifier::None;
    if ((meta_state & kMetaShiftOn) != 0) {
        modifiers = modifiers | ui::Modifier::Shift;
    }
    if ((meta_state & kMetaCtrlOn) != 0) {
        modifiers = modifiers | ui::Modifier::Control;
    }
    if ((meta_state & kMetaAltOn) != 0) {
        modifiers = modifiers | ui::Modifier::Alt;
    }
    if ((meta_state & kMetaMetaOn) != 0) {
        modifiers = modifiers | ui::Modifier::Meta;
    }
    if ((meta_state & kMetaCapsLockOn) != 0) {
        modifiers = modifiers | ui::Modifier::CapsLock;
    }
    if ((meta_state & kMetaNumLockOn) != 0) {
        modifiers = modifiers | ui::Modifier::NumLock;
    }
    return modifiers;
}

ui::PointerKind pointer_kind_from(std::int32_t tool_type, std::int32_t source) {
    switch (tool_type) {
    case kToolStylus:
    case kToolEraser:
        return ui::PointerKind::Pen;
    case kToolMouse:
        return ui::PointerKind::Mouse;
    case kToolFinger:
        return ui::PointerKind::Touch;
    default:
        break;
    }
    // No usable tool type: fall back to the source. A mouse or trackpad is a mouse; a stylus source
    // is a pen; everything else (touchscreen, trackball, an unknown source) is touch, which is the
    // safe answer because touch cannot hover.
    if (source == kSourceMouse || source == kSourceTouchpad) {
        return ui::PointerKind::Mouse;
    }
    if (source == kSourceStylus) {
        return ui::PointerKind::Pen;
    }
    return ui::PointerKind::Touch;
}

ui::KeyCode logical_key_from_keycode(std::int32_t keycode) {
    switch (keycode) {
    case kKeycodeEnter:
        return ui::key::kEnter;
    case kKeycodeTab:
        return ui::key::kTab;
    case kKeycodeSpace:
        return ui::key::kSpace;
    case kKeycodeDel:
    case kKeycodeForwardDel:
        return ui::key::kBackspace;
    case kKeycodeEscape:
        return ui::key::kEscape;
    case kKeycodeDpadLeft:
        return ui::key::kArrowLeft;
    case kKeycodeDpadUp:
        return ui::key::kArrowUp;
    case kKeycodeDpadRight:
        return ui::key::kArrowRight;
    case kKeycodeDpadDown:
        return ui::key::kArrowDown;
    case kKeycodeMoveHome:
        return ui::key::kHome;
    case kKeycodeMoveEnd:
        return ui::key::kEnd;
    case kKeycodePageUp:
        return ui::key::kPageUp;
    case kKeycodePageDown:
        return ui::key::kPageDown;
    case kKeycodeInsert:
        return ui::key::kInsert;
    case kKeycodeShiftLeft:
    case kKeycodeShiftRight:
        return ui::key::kShift;
    case kKeycodeCtrlLeft:
    case kKeycodeCtrlRight:
        return ui::key::kControl;
    case kKeycodeAltLeft:
    case kKeycodeAltRight:
        return ui::key::kAlt;
    case kKeycodeMetaLeft:
    case kKeycodeMetaRight:
        return ui::key::kMeta;
    case kKeycodeCapsLock:
        return ui::key::kCapsLock;
    default:
        break;
    }
    // Letters: AKEYCODE_A..AKEYCODE_Z are contiguous, so the offset is the letter.
    if (keycode >= kKeycodeA && keycode <= kKeycodeZ) {
        return static_cast<ui::KeyCode>('A' + (keycode - kKeycodeA));
    }
    // Digits: AKEYCODE_0..AKEYCODE_9 are contiguous.
    if (keycode >= kKeycode0 && keycode <= kKeycode9) {
        return static_cast<ui::KeyCode>('0' + (keycode - kKeycode0));
    }
    // Function keys: AKEYCODE_F1..AKEYCODE_F12 are contiguous.
    if (keycode >= kKeycodeF1 && keycode <= kKeycodeF12) {
        return static_cast<ui::KeyCode>(ui::key::kF1 + (keycode - kKeycodeF1));
    }
    return ui::key::kUnknown;
}

std::uint32_t codepoint_from_key(std::int32_t keycode, ui::Modifier modifiers) {
    // The NDK has no unicode-char accessor, so the character is derived from the keycode. Letters
    // and digits are the only keys whose typed character is unambiguous without a layout table; the
    // shift state decides the case for letters.
    const bool shift = ui::has_modifier(modifiers, ui::Modifier::Shift);
    if (keycode >= kKeycodeA && keycode <= kKeycodeZ) {
        const auto letter = static_cast<std::uint32_t>('a' + (keycode - kKeycodeA));
        return shift ? letter - ('a' - 'A') : letter;
    }
    if (keycode >= kKeycode0 && keycode <= kKeycode9) {
        // The unshifted digit. The shifted symbols (!@#$...) are layout-dependent and are left to
        // the IME bridge rather than guessed.
        if (shift) {
            return 0;
        }
        return static_cast<std::uint32_t>('0' + (keycode - kKeycode0));
    }
    if (keycode == kKeycodeSpace) {
        return ' ';
    }
    // Everything else (punctuation, non-ASCII, composed text) comes from the IME bridge, not from a
    // keycode guess.
    return 0;
}

MotionAction motion_action(std::int32_t action) {
    MotionAction result;
    const std::int32_t masked = action & kActionMask;
    // The pointer actions carry the pointer index in the high byte.
    if (masked == kActionPointerDown || masked == kActionPointerUp) {
        result.pointer_index = (action >> kActionPointerIndexShift) & kActionMask;
    }
    switch (masked) {
    case kActionDown:
        result.kind = MotionAction::Kind::Down;
        break;
    case kActionUp:
        result.kind = MotionAction::Kind::Up;
        break;
    case kActionMove:
        result.kind = MotionAction::Kind::Move;
        break;
    case kActionCancel:
        result.kind = MotionAction::Kind::Cancel;
        break;
    case kActionOutside:
        // A press that started inside and left the window: the gesture is over, and the engine must
        // not keep the capture. Treated as a cancel, which is what the platform means by it.
        result.kind = MotionAction::Kind::Cancel;
        break;
    case kActionPointerDown:
        result.kind = MotionAction::Kind::Down;
        break;
    case kActionPointerUp:
        result.kind = MotionAction::Kind::Up;
        break;
    case kActionHoverMove:
        result.kind = MotionAction::Kind::HoverMove;
        break;
    case kActionHoverEnter:
        result.kind = MotionAction::Kind::HoverEnter;
        break;
    case kActionHoverExit:
        result.kind = MotionAction::Kind::HoverExit;
        break;
    case kActionScroll:
        result.kind = MotionAction::Kind::Scroll;
        break;
    default:
        result.kind = MotionAction::Kind::Ignored;
        break;
    }
    return result;
}

bool action_cancels_gesture(std::int32_t action) {
    const std::int32_t masked = action & kActionMask;
    return masked == kActionCancel || masked == kActionOutside;
}

float scroll_dy_from_axis(float axis_vscroll) {
    // Android's AXIS_VSCROLL is positive when the content should move up (the finger moved down),
    // which is the opposite of the web sign the engine uses. The horizontal axis already matches.
    return -axis_vscroll;
}

} // namespace tessera::hal::android
