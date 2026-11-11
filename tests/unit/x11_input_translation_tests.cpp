// SPDX-License-Identifier: MIT
//
// Pure X11 input mapping rules (keysym, button, modifier state): platform independent, no display.

#include "tessera/ui/key_code.hpp"
#include "x11_input_translation.hpp"

#include <cstdint>
#include <iostream>

namespace {

namespace x11 = tessera::hal::x11;
namespace ui = tessera::ui;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "x11_input_translation: " << message << "\n";
    }
    return condition;
}

bool modifiers() {
    const ui::Modifier all =
        x11::modifiers_from_state(x11::kShiftMask | x11::kControlMask | x11::kMod1Mask |
                                  x11::kMod4Mask | x11::kLockMask | x11::kMod2Mask);
    return expect(x11::modifiers_from_state(0) == ui::Modifier::None, "no state is no modifiers") &&
           expect(ui::has_modifier(all, ui::Modifier::Shift) &&
                      ui::has_modifier(all, ui::Modifier::Control) &&
                      ui::has_modifier(all, ui::Modifier::Alt) &&
                      ui::has_modifier(all, ui::Modifier::Meta) &&
                      ui::has_modifier(all, ui::Modifier::CapsLock) &&
                      ui::has_modifier(all, ui::Modifier::NumLock),
                  "every state bit maps to its modifier") &&
           expect(x11::modifiers_from_state(x11::kMod1Mask) == ui::Modifier::Alt,
                  "Mod1 is Alt alone");
}

bool logical_keys() {
    return expect(x11::logical_key_from_keysym('a') == 'A',
                  "lower-case letter is upper-case key") &&
           expect(x11::logical_key_from_keysym('Z') == 'Z', "upper-case letter is unchanged") &&
           expect(x11::logical_key_from_keysym('7') == '7', "digit maps to itself") &&
           expect(x11::logical_key_from_keysym(0xFFB3) == '3', "keypad digit maps to the digit") &&
           expect(x11::logical_key_from_keysym(0xFF08) == ui::key::kBackspace, "BackSpace") &&
           expect(x11::logical_key_from_keysym(0xFF0D) == ui::key::kEnter, "Return") &&
           expect(x11::logical_key_from_keysym(0xFF8D) == ui::key::kEnter, "keypad Enter") &&
           expect(x11::logical_key_from_keysym(0x20) == ui::key::kSpace, "space") &&
           expect(x11::logical_key_from_keysym(0xFFFF) == ui::key::kDelete, "Delete") &&
           expect(x11::logical_key_from_keysym(0xFF51) == ui::key::kArrowLeft, "Left") &&
           expect(x11::logical_key_from_keysym(0xFF54) == ui::key::kArrowDown, "Down") &&
           expect(x11::logical_key_from_keysym(0xFF55) == ui::key::kPageUp, "Prior is PageUp") &&
           expect(x11::logical_key_from_keysym(0xFFBE) == ui::key::kF1, "F1") &&
           expect(x11::logical_key_from_keysym(0xFFC9) == ui::key::kF1 + 11, "F12") &&
           expect(x11::logical_key_from_keysym(0xFFE2) == ui::key::kShift, "right Shift") &&
           expect(x11::logical_key_from_keysym(0xFFEC) == ui::key::kMeta, "right Super is Meta") &&
           expect(x11::logical_key_from_keysym(0xFFE5) == ui::key::kCapsLock, "Caps_Lock") &&
           expect(x11::logical_key_from_keysym(0x1000041) == ui::key::kUnknown,
                  "Unicode keysym has no logical key") &&
           expect(x11::logical_key_from_keysym(0) == ui::key::kUnknown, "NoSymbol is unknown");
}

bool codepoints() {
    return expect(x11::codepoint_from_keysym('a') == 'a', "ASCII types itself") &&
           expect(x11::codepoint_from_keysym(0x20) == 0x20, "space types a space") &&
           expect(x11::codepoint_from_keysym(0xE9) == 0xE9, "Latin-1 types itself") &&
           expect(x11::codepoint_from_keysym(0xFFB5) == '5', "keypad digit types the digit") &&
           expect(x11::codepoint_from_keysym(0x1000000 + 0x1EA1) == 0x1EA1,
                  "Unicode keysym carries its scalar") &&
           expect(x11::codepoint_from_keysym(0x1000000 + 0x1F600) == 0x1F600,
                  "supplementary Unicode keysym") &&
           expect(x11::codepoint_from_keysym(0x1000000 + 0xD800) == 0, "surrogate types nothing") &&
           expect(x11::codepoint_from_keysym(0x1110000) == 0, "beyond Unicode types nothing") &&
           expect(x11::codepoint_from_keysym(0xFF08) == 0, "BackSpace types nothing") &&
           expect(x11::codepoint_from_keysym(0xFF51) == 0, "arrows type nothing") &&
           expect(x11::codepoint_from_keysym(0x7F) == 0, "DEL types nothing");
}

bool buttons() {
    const x11::ButtonAction left = x11::button_action(1);
    const x11::ButtonAction up = x11::button_action(4);
    const x11::ButtonAction down = x11::button_action(5);
    const x11::ButtonAction wheel_left = x11::button_action(6);
    const x11::ButtonAction wheel_right = x11::button_action(7);
    return expect(left.kind == x11::ButtonAction::Kind::Button &&
                      left.button == ui::PointerButton::Left,
                  "button 1 is Left") &&
           expect(x11::button_action(2).button == ui::PointerButton::Middle,
                  "button 2 is Middle") &&
           expect(x11::button_action(3).button == ui::PointerButton::Right, "button 3 is Right") &&
           expect(x11::button_action(8).button == ui::PointerButton::Back, "button 8 is Back") &&
           expect(x11::button_action(9).button == ui::PointerButton::Forward,
                  "button 9 is Forward") &&
           expect(up.kind == x11::ButtonAction::Kind::Wheel && up.dy < 0.0F && up.dx == 0.0F,
                  "button 4 scrolls up (negative web dy)") &&
           expect(down.kind == x11::ButtonAction::Kind::Wheel && down.dy > 0.0F,
                  "button 5 scrolls down (positive web dy)") &&
           expect(wheel_left.dx < 0.0F && wheel_left.dy == 0.0F, "button 6 scrolls left") &&
           expect(wheel_right.dx > 0.0F, "button 7 scrolls right") &&
           expect(x11::button_action(0).kind == x11::ButtonAction::Kind::None &&
                      x11::button_action(42).kind == x11::ButtonAction::Kind::None,
                  "unknown buttons are ignored");
}

} // namespace

int main() {
    if (!modifiers()) {
        return 1;
    }
    if (!logical_keys()) {
        return 2;
    }
    if (!codepoints()) {
        return 3;
    }
    if (!buttons()) {
        return 4;
    }
    std::cout << "x11_input_translation: ok\n";
    return 0;
}
