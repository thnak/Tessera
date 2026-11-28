// SPDX-License-Identifier: MIT

// Issue #290: DOM event flattening to normalized InputEvents (ADR-046 item 3). Pure; no browser.

#include "tessera/ui/key_code.hpp"
#include "web_input_translation.hpp"

#include <iostream>

namespace {

using namespace tessera;
using namespace tessera::hal::web;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "web_input_translation: " << message << "\n";
        ++g_failures;
    }
}

void pointer_cases() {
    DomPointerSample down;
    down.phase = DomPointerPhase::Down;
    down.type = DomPointerType::Mouse;
    down.pointer_id = 7;
    down.primary = true;
    down.button = 2;
    down.x = 10.5F;
    down.y = 20.0F;
    down.pressure = 0.5F;
    down.modifiers = kModShift | kModAlt;
    down.timestamp_ms = 1500.0;
    auto event = make_pointer_event(down);
    expect(event.type == ui::InputEventType::PointerDown, "down type");
    expect(event.button == ui::PointerButton::Right, "button 2 is right");
    expect(event.x == 10.5F && event.y == 20.0F, "coordinates are CSS pixels, untouched");
    expect(event.timestamp_s == 1.5, "timestamp converts to seconds");
    expect(event.pressure == 0.0F, "a mouse never reports pressure");
    expect(event.pointer_id == 7 && event.is_primary, "id and primary carried");
    expect(ui::has_modifier(event.modifiers, ui::Modifier::Shift) &&
               ui::has_modifier(event.modifiers, ui::Modifier::Alt) &&
               !ui::has_modifier(event.modifiers, ui::Modifier::Control),
           "modifier bits");

    DomPointerSample move = down;
    move.phase = DomPointerPhase::Move;
    move.button = 0; // a move while a button is held still reports no button change
    expect(make_pointer_event(move).button == ui::PointerButton::None, "move changes no button");

    DomPointerSample pen = down;
    pen.type = DomPointerType::Pen;
    pen.pressure = 1.7F;
    event = make_pointer_event(pen);
    expect(event.pointer_kind == ui::PointerKind::Pen && event.pressure == 1.0F,
           "pen pressure is clamped");

    DomPointerSample touch = down;
    touch.type = DomPointerType::Touch;
    touch.button = -1;
    event = make_pointer_event(touch);
    expect(event.pointer_kind == ui::PointerKind::Touch, "touch kind");
    expect(event.button == ui::PointerButton::Left, "a touch contact is the left button");

    DomPointerSample cancel = down;
    cancel.phase = DomPointerPhase::Cancel;
    expect(make_pointer_event(cancel).type == ui::InputEventType::PointerCancel, "cancel maps");

    DomPointerSample unknown = down;
    unknown.type = DomPointerType::Unknown;
    expect(make_pointer_event(unknown).pointer_kind == ui::PointerKind::Mouse,
           "unknown pointer types keep mouse behavior");

    expect(button_from_dom(1) == ui::PointerButton::Middle &&
               button_from_dom(3) == ui::PointerButton::Back &&
               button_from_dom(4) == ui::PointerButton::Forward &&
               button_from_dom(9) == ui::PointerButton::Other,
           "button table");
}

void wheel_cases() {
    DomWheelSample wheel;
    wheel.delta_y = 100.0F;
    wheel.delta_x = -50.0F;
    auto event = make_wheel_event(wheel);
    expect(event.type == ui::InputEventType::Wheel, "wheel type");
    expect(event.dy == 1.0F && event.dx == -0.5F, "100 px is one notch, web sign");
    wheel.mode = DomWheelMode::Line;
    wheel.delta_y = 3.0F;
    wheel.delta_x = 0.0F;
    expect(make_wheel_event(wheel).dy == 1.0F, "3 lines is one notch");
    wheel.mode = DomWheelMode::Page;
    wheel.delta_y = 1.0F;
    expect(make_wheel_event(wheel).dy == kNotchesPerPage, "a page is ten notches");
}

void key_cases() {
    expect(key_code_from_dom("a") == 'A' && key_code_from_dom("Z") == 'Z' &&
               key_code_from_dom("7") == '7',
           "letters upper-case, digits stay");
    expect(key_code_from_dom("Enter") == ui::key::kEnter &&
               key_code_from_dom(" ") == ui::key::kSpace &&
               key_code_from_dom("ArrowLeft") == ui::key::kArrowLeft,
           "named keys");
    expect(key_code_from_dom("F1") == ui::key::kF1 &&
               key_code_from_dom("F12") == ui::key::kF1 + 11U,
           "function keys");
    expect(key_code_from_dom("F13") == ui::key::kUnknown && key_code_from_dom("F") == 'F' &&
               key_code_from_dom("Dead") == ui::key::kUnknown &&
               key_code_from_dom("!") == ui::key::kUnknown,
           "unmapped keys are unknown");

    const auto none = ui::Modifier::None;
    expect(typed_codepoint("a", none) == 'a', "a letter types itself");
    expect(typed_codepoint(" ", none) == ' ', "space types");
    expect(typed_codepoint("Enter", none) == 0 && typed_codepoint("ArrowUp", none) == 0,
           "named keys type nothing");
    expect(typed_codepoint("a", ui::Modifier::Control) == 0 &&
               typed_codepoint("a", ui::Modifier::Meta) == 0,
           "shortcuts are not text");
    expect(typed_codepoint("@", ui::Modifier::Control | ui::Modifier::Alt) == '@', "AltGr types");
    expect(typed_codepoint("\xC3\xA9", none) == 0xE9, "two-byte scalar");
    expect(typed_codepoint("\xE2\x82\xAC", none) == 0x20AC, "three-byte scalar");
    expect(typed_codepoint("\xF0\x9F\x98\x80", none) == 0x1F600, "four-byte scalar");
    expect(typed_codepoint("\xC3", none) == 0xFFFD, "truncated sequence yields U+FFFD");
    expect(typed_codepoint("\xC0\x80", none) == 0xFFFD, "overlong sequence yields U+FFFD");
    expect(typed_codepoint("\xED\xA0\x80", none) == 0xFFFD, "surrogate yields U+FFFD");
    expect(typed_codepoint("", none) == 0, "empty key");

    DomKeySample key;
    key.key = "b";
    key.repeat = true;
    key.modifiers = kModControl | kModCapsLock;
    auto event = make_key_event(key);
    expect(event.type == ui::InputEventType::KeyDown && event.key == 'B' && event.repeat,
           "key down");
    expect(ui::has_modifier(event.modifiers, ui::Modifier::CapsLock), "caps lock bit");
    ui::InputEvent text;
    expect(!make_text_event(key, text), "ctrl+b makes no text");
    key.modifiers = 0;
    expect(make_text_event(key, text) && text.type == ui::InputEventType::Text &&
               text.text_codepoint == 'b',
           "plain b makes text");
    key.down = false;
    event = make_key_event(key);
    expect(event.type == ui::InputEventType::KeyUp && !event.repeat, "key up carries no repeat");
    expect(!make_text_event(key, text), "key up makes no text");
}

} // namespace

int main() {
    pointer_cases();
    wheel_cases();
    key_cases();
    return g_failures == 0 ? 0 : 1;
}
