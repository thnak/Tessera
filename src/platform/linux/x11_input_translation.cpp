// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "x11_input_translation.hpp"

namespace tessera::hal::x11 {
namespace {

constexpr unsigned long kXkBackSpace = 0xFF08;
constexpr unsigned long kXkTab = 0xFF09;
constexpr unsigned long kXkReturn = 0xFF0D;
constexpr unsigned long kXkEscape = 0xFF1B;
constexpr unsigned long kXkHome = 0xFF50;
constexpr unsigned long kXkLeft = 0xFF51;
constexpr unsigned long kXkUp = 0xFF52;
constexpr unsigned long kXkRight = 0xFF53;
constexpr unsigned long kXkDown = 0xFF54;
constexpr unsigned long kXkPrior = 0xFF55;
constexpr unsigned long kXkNext = 0xFF56;
constexpr unsigned long kXkEnd = 0xFF57;
constexpr unsigned long kXkInsert = 0xFF63;
constexpr unsigned long kXkKeypadEnter = 0xFF8D;
constexpr unsigned long kXkKeypadFirstDigit = 0xFFB0;
constexpr unsigned long kXkKeypadLastDigit = 0xFFB9;
constexpr unsigned long kXkF1 = 0xFFBE;
constexpr unsigned long kXkF12 = 0xFFC9;
constexpr unsigned long kXkShiftLeft = 0xFFE1;
constexpr unsigned long kXkShiftRight = 0xFFE2;
constexpr unsigned long kXkControlLeft = 0xFFE3;
constexpr unsigned long kXkControlRight = 0xFFE4;
constexpr unsigned long kXkCapsLock = 0xFFE5;
constexpr unsigned long kXkMetaLeft = 0xFFE7;
constexpr unsigned long kXkMetaRight = 0xFFE8;
constexpr unsigned long kXkAltLeft = 0xFFE9;
constexpr unsigned long kXkAltRight = 0xFFEA;
constexpr unsigned long kXkSuperLeft = 0xFFEB;
constexpr unsigned long kXkSuperRight = 0xFFEC;
constexpr unsigned long kXkDelete = 0xFFFF;

constexpr unsigned long kXkSpace = 0x20;
constexpr unsigned long kXkAsciiLast = 0x7E;
constexpr unsigned long kXkLatin1First = 0xA0;
constexpr unsigned long kXkLatin1Last = 0xFF;
constexpr unsigned long kXkUnicodeBase = 0x01000000;
constexpr unsigned long kXkUnicodeMax = 0x0110FFFF;

constexpr unsigned kButtonLeft = 1;
constexpr unsigned kButtonMiddle = 2;
constexpr unsigned kButtonRight = 3;
constexpr unsigned kButtonWheelUp = 4;
constexpr unsigned kButtonWheelDown = 5;
constexpr unsigned kButtonWheelLeft = 6;
constexpr unsigned kButtonWheelRight = 7;
constexpr unsigned kButtonBack = 8;
constexpr unsigned kButtonForward = 9;

} // namespace

ui::Modifier modifiers_from_state(unsigned state) {
    ui::Modifier modifiers = ui::Modifier::None;
    if ((state & kShiftMask) != 0) {
        modifiers = modifiers | ui::Modifier::Shift;
    }
    if ((state & kControlMask) != 0) {
        modifiers = modifiers | ui::Modifier::Control;
    }
    if ((state & kMod1Mask) != 0) {
        modifiers = modifiers | ui::Modifier::Alt;
    }
    if ((state & kMod4Mask) != 0) {
        modifiers = modifiers | ui::Modifier::Meta;
    }
    if ((state & kLockMask) != 0) {
        modifiers = modifiers | ui::Modifier::CapsLock;
    }
    if ((state & kMod2Mask) != 0) {
        modifiers = modifiers | ui::Modifier::NumLock;
    }
    return modifiers;
}

ui::KeyCode logical_key_from_keysym(unsigned long keysym) {
    if (keysym >= 'a' && keysym <= 'z') {
        return static_cast<ui::KeyCode>(keysym - 'a' + 'A');
    }
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9')) {
        return static_cast<ui::KeyCode>(keysym);
    }
    if (keysym >= kXkF1 && keysym <= kXkF12) {
        return ui::key::kF1 + static_cast<ui::KeyCode>(keysym - kXkF1);
    }
    if (keysym >= kXkKeypadFirstDigit && keysym <= kXkKeypadLastDigit) {
        return static_cast<ui::KeyCode>('0' + (keysym - kXkKeypadFirstDigit));
    }
    switch (keysym) {
    case kXkBackSpace:
        return ui::key::kBackspace;
    case kXkTab:
        return ui::key::kTab;
    case kXkReturn:
    case kXkKeypadEnter:
        return ui::key::kEnter;
    case kXkEscape:
        return ui::key::kEscape;
    case kXkSpace:
        return ui::key::kSpace;
    case kXkInsert:
        return ui::key::kInsert;
    case kXkDelete:
        return ui::key::kDelete;
    case kXkHome:
        return ui::key::kHome;
    case kXkEnd:
        return ui::key::kEnd;
    case kXkPrior:
        return ui::key::kPageUp;
    case kXkNext:
        return ui::key::kPageDown;
    case kXkLeft:
        return ui::key::kArrowLeft;
    case kXkUp:
        return ui::key::kArrowUp;
    case kXkRight:
        return ui::key::kArrowRight;
    case kXkDown:
        return ui::key::kArrowDown;
    case kXkShiftLeft:
    case kXkShiftRight:
        return ui::key::kShift;
    case kXkControlLeft:
    case kXkControlRight:
        return ui::key::kControl;
    case kXkAltLeft:
    case kXkAltRight:
        return ui::key::kAlt;
    case kXkMetaLeft:
    case kXkMetaRight:
    case kXkSuperLeft:
    case kXkSuperRight:
        return ui::key::kMeta;
    case kXkCapsLock:
        return ui::key::kCapsLock;
    default:
        return ui::key::kUnknown;
    }
}

std::uint32_t codepoint_from_keysym(unsigned long keysym) {
    if (keysym >= kXkSpace && keysym <= kXkAsciiLast) {
        return static_cast<std::uint32_t>(keysym);
    }
    if (keysym >= kXkLatin1First && keysym <= kXkLatin1Last) {
        return static_cast<std::uint32_t>(keysym);
    }
    if (keysym >= kXkKeypadFirstDigit && keysym <= kXkKeypadLastDigit) {
        return static_cast<std::uint32_t>('0' + (keysym - kXkKeypadFirstDigit));
    }
    if (keysym > kXkUnicodeBase && keysym <= kXkUnicodeMax) {
        const auto codepoint = static_cast<std::uint32_t>(keysym - kXkUnicodeBase);
        // Surrogate halves are not scalar values.
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return 0;
        }
        return codepoint;
    }
    return 0;
}

ButtonAction button_action(unsigned x_button) {
    ButtonAction action;
    const auto as_button = [&action](ui::PointerButton button) {
        action.kind = ButtonAction::Kind::Button;
        action.button = button;
    };
    const auto as_wheel = [&action](float dx, float dy) {
        action.kind = ButtonAction::Kind::Wheel;
        action.dx = dx;
        action.dy = dy;
    };
    switch (x_button) {
    case kButtonLeft:
        as_button(ui::PointerButton::Left);
        break;
    case kButtonMiddle:
        as_button(ui::PointerButton::Middle);
        break;
    case kButtonRight:
        as_button(ui::PointerButton::Right);
        break;
    case kButtonWheelUp:
        as_wheel(0.0F, -1.0F);
        break;
    case kButtonWheelDown:
        as_wheel(0.0F, 1.0F);
        break;
    case kButtonWheelLeft:
        as_wheel(-1.0F, 0.0F);
        break;
    case kButtonWheelRight:
        as_wheel(1.0F, 0.0F);
        break;
    case kButtonBack:
        as_button(ui::PointerButton::Back);
        break;
    case kButtonForward:
        as_button(ui::PointerButton::Forward);
        break;
    default:
        break;
    }
    return action;
}

} // namespace tessera::hal::x11
