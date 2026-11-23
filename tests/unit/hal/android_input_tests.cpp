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

    // --- Java KeyEvent translation -----------------------------------------------------------
    //
    // The shell is a Java activity, so a hardware key arrives as a `KeyEvent` and never as the
    // NDK's `AInputEvent`. Without this path the engine sees no Backspace at all and the field
    // cannot be emptied, which is exactly the defect these checks pin.
    {
        android::KeySample backspace;
        backspace.keycode = android::kKeycodeDel;
        backspace.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(backspace);
        check(out.count == 1, "a Backspace key-down produces exactly one event");
        check(out.events[0].type == tessera::ui::InputEventType::KeyDown,
              "the Backspace event is a KeyDown");
        check(out.events[0].key == key::kBackspace, "the Backspace keycode maps to kBackspace");
        // Backspace types nothing, so it must not also emit a Text event: a Text event with a zero
        // codepoint would be dropped by the controller anyway, but emitting one would be a lie.
        check(out.events[0].text_codepoint == 0, "Backspace types no character");
    }
    {
        // A letter key-down produces the key event AND the typed character, which is what the edit
        // controller inserts. Losing the second event is what makes typing appear to do nothing.
        android::KeySample letter;
        letter.keycode = android::kKeycodeA;
        letter.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(letter);
        check(out.count == 2, "a letter key-down produces the key and the text");
        check(out.events[0].type == tessera::ui::InputEventType::KeyDown, "the first is the key");
        check(out.events[1].type == tessera::ui::InputEventType::Text, "the second is the text");
        check(out.events[1].text_codepoint == 'a', "an unshifted letter types lower case");
    }
    {
        // Shift decides the case, and the modifier rides on both events so a handler can tell.
        android::KeySample shifted;
        shifted.keycode = android::kKeycodeA;
        shifted.meta_state = android::kMetaShiftOn;
        shifted.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(shifted);
        check(out.count == 2, "a shifted letter still produces both events");
        check(out.events[1].text_codepoint == 'A', "a shifted letter types upper case");
        check(tessera::ui::has_modifier(out.events[0].modifiers, Modifier::Shift),
              "the key event carries the shift modifier");
    }
    {
        // A chord types NOTHING. Ctrl+A is Select all, not "insert a": without this guard the key
        // produced both a KeyDown (which the edit controller runs as a command) and a Text event
        // (which it inserts), so every chord also typed its letter.
        android::KeySample chord;
        chord.keycode = android::kKeycodeA;
        chord.meta_state = android::kMetaCtrlOn;
        chord.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(chord);
        check(out.count == 1, "a Ctrl chord produces only the key event");
        check(out.events[0].type == tessera::ui::InputEventType::KeyDown,
              "the chord's one event is the key");
        check(tessera::ui::has_modifier(out.events[0].modifiers, Modifier::Control),
              "the chord carries Control");
    }
    {
        // Alt and Meta are chords too: an Alt+letter belongs to the app, not to the field.
        android::KeySample alt;
        alt.keycode = android::kKeycodeA;
        alt.meta_state = android::kMetaAltOn;
        alt.action = android::kKeyActionDown;
        check(android::translate_key_sample(alt).count == 1, "an Alt chord types nothing");
        android::KeySample meta;
        meta.keycode = android::kKeycodeA;
        meta.meta_state = android::kMetaMetaOn;
        meta.action = android::kKeyActionDown;
        check(android::translate_key_sample(meta).count == 1, "a Meta chord types nothing");
    }
    {
        // A key-up never types: the character was already committed by the key-down, and emitting
        // it twice would double every keystroke.
        android::KeySample up;
        up.keycode = android::kKeycodeA;
        up.action = android::kKeyActionUp;
        const android::KeyTranslation out = android::translate_key_sample(up);
        check(out.count == 1, "a key-up produces only the key event");
        check(out.events[0].type == tessera::ui::InputEventType::KeyUp, "the event is a KeyUp");
    }
    {
        // A key the engine has no logical code for produces nothing, so the caller can let the
        // platform handle it. This is what keeps the system Back gesture working.
        android::KeySample unknown;
        unknown.keycode = android::kKeycodeBack;
        unknown.action = android::kKeyActionDown;
        check(android::translate_key_sample(unknown).count == 0,
              "an unmapped key produces no event");
    }
    {
        // A chord's multiple-action event is not a single transition; the platform sends the
        // individual events too.
        android::KeySample multiple;
        multiple.keycode = android::kKeycodeA;
        multiple.action = android::kKeyActionMultiple;
        check(android::translate_key_sample(multiple).count == 0,
              "a multiple-action event produces no event");
    }
    {
        // The repeat count rides through, so auto-repeat is distinguishable from a fresh press.
        android::KeySample repeat;
        repeat.keycode = android::kKeycodeDel;
        repeat.action = android::kKeyActionDown;
        repeat.repeat_count = 3;
        const android::KeyTranslation out = android::translate_key_sample(repeat);
        check(out.count == 1 && out.events[0].repeat, "the repeat count reaches the event");
    }
    {
        // Space types a space. It is the one non-letter key whose character is unambiguous, and an
        // IME that synthesises it as a key event (rather than committing text) is common: without
        // this the field cannot contain a space at all.
        android::KeySample space;
        space.keycode = android::kKeycodeSpace;
        space.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(space);
        check(out.count == 2, "space produces the key and the text");
        check(out.events[0].key == key::kSpace, "the space keycode maps to kSpace");
        check(out.events[1].text_codepoint == ' ', "space types a space");
    }
    {
        // Enter maps to the newline chord. It types no character: the edit controller's key map
        // turns the key event into `InsertNewline`, which a single-line field gates off and a
        // multi-line field applies. Emitting a '\n' text event as well would insert two newlines.
        android::KeySample enter;
        enter.keycode = android::kKeycodeEnter;
        enter.action = android::kKeyActionDown;
        const android::KeyTranslation out = android::translate_key_sample(enter);
        check(out.count == 1, "enter produces only the key event");
        check(out.events[0].key == key::kEnter, "the enter keycode maps to kEnter");
        check(out.events[0].text_codepoint == 0, "enter types no character");
    }

    if (failures != 0) {
        std::cerr << "android_input: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_input: ok\n";
    return 0;
}
