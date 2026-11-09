// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "tessera/hal/window.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"
#include "win32_pointer_translation.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

namespace {

using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputQueue;
using tessera::ui::PointerButton;
using tessera::ui::PointerKind;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_win32_input: " << message << "\n";
    }
    return condition;
}

bool approx(float value, float expected) {
    return value > expected - 0.01f && value < expected + 0.01f;
}

std::vector<InputEvent> drain(InputQueue& queue) {
    std::vector<InputEvent> events;
    InputEvent event;
    while (queue.pop(event)) {
        events.push_back(event);
    }
    return events;
}

LPARAM point(int x, int y) {
    return MAKELPARAM(x, y);
}

WPARAM wheel(int delta, WORD keys = 0) {
    return MAKEWPARAM(keys, static_cast<WORD>(static_cast<short>(delta)));
}

} // namespace

int main() {
    const std::string title = "Tessera win32 input test " + std::to_string(GetCurrentProcessId());
    tessera::hal::WindowDesc desc;
    desc.title = title;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;
    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 1;
    }
    const HWND hwnd = FindWindowA(nullptr, title.c_str());
    if (!expect(hwnd != nullptr, "native window exists")) {
        return 2;
    }
    const float scale = window->metrics().dpi_scale;

    InputQueue queue(64);

    // Without a queue input is dropped instead of crashing.
    SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 1);
    SendMessageW(hwnd, WM_CHAR, 'x', 1);
    window->set_input_queue(&queue);
    if (!expect(queue.empty(), "nothing is queued before a queue is set")) {
        return 3;
    }

    // Pointer moves are in logical pixels, with the delta from the previous position.
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point(40, 60));
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_CONTROL, point(50, 60));
    auto events = drain(queue);
    if (!expect(events.size() == 2, "two moves queue two events") ||
        !expect(events[0].type == InputEventType::PointerMove, "move type") ||
        !expect(approx(events[0].x, 40.0f / scale) && approx(events[0].y, 60.0f / scale),
                "move position is logical") ||
        !expect(approx(events[0].dx, 0.0f), "first move has no delta") ||
        !expect(approx(events[1].dx, 10.0f / scale) && approx(events[1].dy, 0.0f),
                "second move carries the delta") ||
        !expect(events[0].button == PointerButton::None && events[0].pressure == 0.0f,
                "idle move has no button or pressure") ||
        !expect(tessera::ui::has_modifier(events[1].modifiers, tessera::ui::Modifier::Control),
                "control from wParam") ||
        !expect(events[0].is_primary, "mouse is the primary pointer")) {
        return 4;
    }

    // A press captures the pointer; the release gives it back.
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, point(40, 60));
    const bool captured = GetCapture() == hwnd;
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point(41, 60));
    const bool released = GetCapture() != hwnd;
    events = drain(queue);
    if (!expect(captured, "press captures the mouse") ||
        !expect(released, "release gives the mouse back") ||
        !expect(events.size() == 2, "press and release queue two events") ||
        !expect(events[0].type == InputEventType::PointerDown &&
                    events[0].button == PointerButton::Left,
                "left press") ||
        !expect(approx(events[0].pressure, 0.5f), "active press has pressure") ||
        !expect(tessera::ui::has_modifier(events[0].modifiers, tessera::ui::Modifier::Shift),
                "shift from wParam") ||
        !expect(events[1].type == InputEventType::PointerUp &&
                    events[1].button == PointerButton::Left,
                "left release") ||
        !expect(events[1].pressure == 0.0f, "release has no pressure")) {
        return 5;
    }

    // Right, middle and the two side buttons map to their own buttons.
    SendMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, point(1, 1));
    SendMessageW(hwnd, WM_RBUTTONUP, 0, point(1, 1));
    SendMessageW(hwnd, WM_MBUTTONDOWN, MK_MBUTTON, point(1, 1));
    SendMessageW(hwnd, WM_MBUTTONUP, 0, point(1, 1));
    const LRESULT back_result =
        SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), point(1, 1));
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), point(1, 1));
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), point(1, 1));
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), point(1, 1));
    events = drain(queue);
    if (!expect(events.size() == 8, "four buttons queue eight events") ||
        !expect(events[0].button == PointerButton::Right, "right button") ||
        !expect(events[2].button == PointerButton::Middle, "middle button") ||
        !expect(events[4].button == PointerButton::Back, "back button") ||
        !expect(events[6].button == PointerButton::Forward, "forward button") ||
        !expect(back_result == TRUE, "X button messages report handled")) {
        return 6;
    }

    // A release without a press is ignored; losing capture cancels the press.
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point(1, 1));
    if (!expect(queue.empty(), "stray release is ignored")) {
        return 7;
    }
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point(7, 8));
    ReleaseCapture(); // sends WM_CAPTURECHANGED to the window
    SendMessageW(hwnd, WM_LBUTTONUP, 0, point(7, 8));
    events = drain(queue);
    if (!expect(events.size() == 2, "press then capture loss queue two events") ||
        !expect(events[1].type == InputEventType::PointerCancel, "capture loss cancels") ||
        !expect(approx(events[1].x, 7.0f / scale), "cancel keeps the last position")) {
        return 8;
    }

    // Losing focus cancels a press and reports Blur; gaining it reports Focus.
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point(2, 2));
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    events = drain(queue);
    if (!expect(events.size() == 4, "press, cancel, blur, focus") ||
        !expect(events[1].type == InputEventType::PointerCancel, "blur cancels the press") ||
        !expect(events[2].type == InputEventType::Blur, "blur event") ||
        !expect(events[3].type == InputEventType::Focus, "focus event")) {
        return 9;
    }

    // Wheel messages carry screen coordinates; deltas are notches with the web sign.
    POINT wheel_point{30, 40};
    ClientToScreen(hwnd, &wheel_point);
    const LPARAM screen = point(wheel_point.x, wheel_point.y);
    SendMessageW(hwnd, WM_MOUSEWHEEL, wheel(120), screen);
    SendMessageW(hwnd, WM_MOUSEWHEEL, wheel(-240), screen);
    SendMessageW(hwnd, WM_MOUSEHWHEEL, wheel(120), screen);
    events = drain(queue);
    if (!expect(events.size() == 3, "three wheel messages") ||
        !expect(events[0].type == InputEventType::Wheel, "wheel type") ||
        !expect(approx(events[0].x, 30.0f / scale) && approx(events[0].y, 40.0f / scale),
                "wheel position is client logical") ||
        !expect(approx(events[0].dy, -1.0f) && approx(events[0].dx, 0.0f), "wheel up scrolls up") ||
        !expect(approx(events[1].dy, 2.0f), "wheel down two notches") ||
        !expect(approx(events[2].dx, 1.0f) && approx(events[2].dy, 0.0f), "horizontal wheel")) {
        return 10;
    }

    // Keys map to logical codes; auto-repeat is flagged on key down only.
    SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 1 | (1 << 30));
    SendMessageW(hwnd, WM_KEYUP, VK_TAB, 1 | (1 << 30) | (1 << 31));
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_F2, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 1);
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RIGHT, 1 | (1 << 29));
    events = drain(queue);
    if (!expect(events.size() == 7, "seven key messages") ||
        !expect(events[0].type == InputEventType::KeyDown &&
                    events[0].key == tessera::ui::key::kTab && !events[0].repeat,
                "tab down") ||
        !expect(events[1].repeat, "auto-repeat is flagged") ||
        !expect(events[2].type == InputEventType::KeyUp && !events[2].repeat, "key up") ||
        !expect(events[3].key == static_cast<tessera::ui::KeyCode>('A'), "letters use ASCII") ||
        !expect(events[4].key == tessera::ui::key::kF1 + 1, "F2") ||
        !expect(events[5].key == tessera::ui::key::kArrowLeft, "arrow left") ||
        !expect(events[6].key == tessera::ui::key::kArrowRight, "system key is recorded")) {
        return 11;
    }

    // Characters arrive as Text, with surrogate pairs combined and controls left to key events.
    SendMessageW(hwnd, WM_CHAR, 'x', 1);
    SendMessageW(hwnd, WM_CHAR, 8, 1);      // backspace
    SendMessageW(hwnd, WM_CHAR, 0x7F, 1);   // delete
    SendMessageW(hwnd, WM_CHAR, 0xD83D, 1); // high half of U+1F600
    SendMessageW(hwnd, WM_CHAR, 0xDE00, 1); // low half
    SendMessageW(hwnd, WM_CHAR, 0xDE00, 1); // orphan low half
    SendMessageW(hwnd, WM_CHAR, 0xD83D, 1); // high half followed by a plain character
    SendMessageW(hwnd, WM_CHAR, 'a', 1);
    SendMessageW(hwnd, WM_CHAR, 0x00E9, 1); // e with acute, a BMP character
    events = drain(queue);
    if (!expect(events.size() == 4, "four text events") ||
        !expect(events[0].type == InputEventType::Text && events[0].text_codepoint == 'x',
                "plain character") ||
        !expect(events[1].text_codepoint == 0x1F600, "surrogate pair is combined") ||
        !expect(events[2].text_codepoint == 'a', "orphan high half is dropped") ||
        !expect(events[3].text_codepoint == 0x00E9, "BMP character")) {
        return 12;
    }

    // A wait wakes for unconsumed input and idles when there is none.
    std::array<tessera::hal::WindowEvent, 32> window_events{};
    while (window->poll_events(window_events) != 0) {
    }
    if (!expect(!window->wait_for_events(0.02), "an empty queue lets the wait time out")) {
        return 13;
    }
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, point(3, 3));
    if (!expect(window->wait_for_events(1.0), "posted input wakes the wait") ||
        !expect(!queue.empty(), "the woken wait queued the input")) {
        return 14;
    }
    if (!expect(window->wait_for_events(0.0), "unconsumed input keeps waits returning")) {
        return 15;
    }
    drain(queue);

    // Overflow sets the queue's flag and never crashes.
    InputQueue tiny(2);
    window->set_input_queue(&tiny);
    for (int i = 0; i < 10; ++i) {
        SendMessageW(hwnd, WM_MOUSEMOVE, 0, point(i, i));
    }
    if (!expect(tiny.overflowed(), "a full queue reports overflow")) {
        return 16;
    }

    window->set_input_queue(nullptr);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point(9, 9));
    if (!expect(drain(queue).empty(), "resetting the queue stops delivery")) {
        return 17;
    }

    // Pointer messages for ids Windows no longer knows are dropped unless they are a cancel.
    window->set_input_queue(&queue);
    SendMessageW(hwnd, WM_POINTERDOWN, 9999, 0);
    if (!expect(drain(queue).empty(), "unknown pointer id produces no event")) {
        return 18;
    }
    SendMessageW(hwnd, WM_POINTERCAPTURECHANGED, 9999, 0);
    events = drain(queue);
    if (!expect(events.size() == 1, "pointer cancel queues one event") ||
        !expect(events[0].type == InputEventType::PointerCancel, "capture loss cancels") ||
        !expect(events[0].pointer_id == 9999, "cancel keeps the pointer id")) {
        return 19;
    }
    window->set_input_queue(nullptr);
    window.reset();

    // Pure translation decisions, no HWND or device involved.
    using tessera::hal::win32::kTouchPointerIdBase;
    using tessera::hal::win32::make_pointer_event;
    using tessera::hal::win32::make_touch_event;
    using tessera::hal::win32::pointer_kind_of;
    using tessera::hal::win32::PointerMessage;
    using tessera::hal::win32::PointerSample;
    using tessera::hal::win32::PointerType;
    using tessera::hal::win32::touch_pointer_id;
    using tessera::hal::win32::TouchSample;

    if (!expect(pointer_kind_of(PointerType::Mouse) == PointerKind::Mouse, "mouse type kind") ||
        !expect(pointer_kind_of(PointerType::Touch) == PointerKind::Touch, "touch type kind") ||
        !expect(pointer_kind_of(PointerType::Pen) == PointerKind::Pen, "pen type kind") ||
        !expect(pointer_kind_of(PointerType::Unknown) == PointerKind::Mouse,
                "unknown pointer types keep the mouse kind")) {
        return 20;
    }

    PointerSample pen;
    pen.type = PointerType::Pen;
    pen.pointer_id = 42;
    pen.primary = true;
    pen.pressure = 512;
    pen.client_x = 20.0f;
    pen.client_y = 10.0f;
    pen.message = PointerMessage::Down;
    pen.button = PointerButton::Left;
    InputEvent event;
    if (!expect(make_pointer_event(pen, 2.0f, event), "pen down translates") ||
        !expect(event.type == InputEventType::PointerDown, "pen down type") ||
        !expect(event.pointer_id == 42, "pointer id preserved") ||
        !expect(event.pointer_kind == PointerKind::Pen, "pen down kind") ||
        !expect(event.is_primary, "primary flag propagated") ||
        !expect(approx(event.pressure, 0.5f), "pen pressure normalized") ||
        !expect(approx(event.x, 10.0f) && approx(event.y, 5.0f), "DPI scale applied") ||
        !expect(event.button == PointerButton::Left, "pen down uses the left button")) {
        return 21;
    }

    pen.pressure = 4096;
    if (!expect(make_pointer_event(pen, 1.0f, event), "pen pressure sample translates") ||
        !expect(approx(event.pressure, 1.0f), "pen pressure clamps to one")) {
        return 22;
    }
    pen.pressure = 0;
    if (!expect(make_pointer_event(pen, 1.0f, event), "zero pressure sample translates") ||
        !expect(event.pressure == 0.0f, "zero pressure stays zero")) {
        return 23;
    }

    PointerSample touch;
    touch.type = PointerType::Touch;
    touch.pointer_id = 7;
    touch.pressure = 512;
    touch.in_contact = true;
    touch.message = PointerMessage::Update;
    if (!expect(make_pointer_event(touch, 1.0f, event), "touch contact update translates") ||
        !expect(event.type == InputEventType::PointerMove, "touch update is a move") ||
        !expect(event.pointer_kind == PointerKind::Touch, "touch move keeps the touch kind") ||
        !expect(event.pressure == 0.0f, "touch pressure is zero")) {
        return 24;
    }

    touch.in_contact = false;
    if (!expect(!make_pointer_event(touch, 1.0f, event), "touch never hovers")) {
        return 25;
    }

    PointerSample pen_hover = pen;
    pen_hover.in_contact = false;
    pen_hover.message = PointerMessage::Update;
    if (!expect(make_pointer_event(pen_hover, 1.0f, event), "pen hover move is allowed") ||
        !expect(event.pointer_kind == PointerKind::Pen, "pen hover kind")) {
        return 26;
    }

    PointerSample cancel = touch;
    cancel.in_contact = false;
    cancel.message = PointerMessage::Cancel;
    if (!expect(make_pointer_event(cancel, 1.0f, event), "cancel always translates") ||
        !expect(event.type == InputEventType::PointerCancel, "cancel maps to PointerCancel")) {
        return 27;
    }
    PointerSample release = pen;
    release.in_contact = true;
    release.message = PointerMessage::Up;
    if (!expect(make_pointer_event(release, 1.0f, event), "pen up translates") ||
        !expect(event.type == InputEventType::PointerUp, "pen up type")) {
        return 28;
    }

    TouchSample contact;
    contact.touch_id = 3;
    contact.primary = true;
    contact.down = true;
    contact.client_x = 30.0f;
    contact.client_y = 40.0f;
    InputEvent touch_down;
    if (!expect(make_touch_event(contact, 2.0f, touch_down), "touch down translates") ||
        !expect(touch_down.type == InputEventType::PointerDown, "touch down type") ||
        !expect(touch_down.pointer_kind == PointerKind::Touch, "touch down kind") ||
        !expect(touch_down.is_primary, "touch primary contact flag") ||
        !expect(touch_down.pointer_id == (kTouchPointerIdBase | 3u),
                "touch id mapped into the fallback space") ||
        !expect((touch_down.pointer_id & kTouchPointerIdBase) != 0u,
                "touch id has the disjoint high bit") ||
        !expect(approx(touch_down.x, 15.0f) && approx(touch_down.y, 20.0f),
                "touch DPI scale applied") ||
        !expect(touch_down.button == PointerButton::Left, "touch down button")) {
        return 29;
    }

    contact.down = false;
    contact.move = true;
    InputEvent touch_move;
    if (!expect(make_touch_event(contact, 1.0f, touch_move), "touch move translates") ||
        !expect(touch_move.type == InputEventType::PointerMove, "touch move type") ||
        !expect(touch_move.pointer_id == touch_down.pointer_id, "touch id is stable on move") ||
        !expect(touch_move.pointer_kind == PointerKind::Touch, "touch move is a contact move") ||
        !expect(touch_move.button == PointerButton::None, "contact move has no button")) {
        return 30;
    }

    contact.move = false;
    contact.up = true;
    InputEvent touch_up;
    if (!expect(make_touch_event(contact, 1.0f, touch_up), "touch up translates") ||
        !expect(touch_up.type == InputEventType::PointerUp, "touch up type") ||
        !expect(touch_up.pointer_id == touch_down.pointer_id, "touch id is stable on up")) {
        return 31;
    }

    contact.up = false;
    if (!expect(!make_touch_event(contact, 1.0f, touch_up), "touch without flags emits nothing")) {
        return 32;
    }
    if (!expect(touch_pointer_id(4) != touch_down.pointer_id,
                "distinct contacts get distinct ids") ||
        !expect(touch_pointer_id(3) == touch_down.pointer_id, "touch id mapping is stable") ||
        !expect(touch_pointer_id(0xFFFFFFFFu) != 0u, "large touch ids stay non-zero")) {
        return 33;
    }

    std::cout << "window_win32_input: ok\n";
    return 0;
}
