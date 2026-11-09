// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/ui/key_code.hpp"
#include "win32_input.hpp"

#include <algorithm>
#include <array>
#include <windowsx.h>

namespace tessera::hal {
namespace {

constexpr UINT kDefaultDpi = 96;
constexpr float kWheelNotch = static_cast<float>(WHEEL_DELTA);
constexpr float kActivePressure = 0.5f;
constexpr std::size_t kMaxTouchContacts = 32;
constexpr wchar_t kHighSurrogateFirst = 0xD800;
constexpr wchar_t kHighSurrogateLast = 0xDBFF;
constexpr wchar_t kLowSurrogateFirst = 0xDC00;
constexpr wchar_t kLowSurrogateLast = 0xDFFF;
constexpr std::uint32_t kSurrogateBase = 0x10000;
constexpr std::uint32_t kSurrogateBits = 10;
constexpr wchar_t kFirstPrintable = 0x20;
constexpr wchar_t kDelete = 0x7F;

win32::PointerType pointer_type_of(POINTER_INPUT_TYPE type) {
    switch (type) {
    case PT_MOUSE:
        return win32::PointerType::Mouse;
    case PT_TOUCH:
        return win32::PointerType::Touch;
    case PT_PEN:
        return win32::PointerType::Pen;
    default:
        return win32::PointerType::Unknown;
    }
}

float dpi_scale_of(HWND window) {
    UINT dpi = GetDpiForWindow(window);
    if (dpi == 0) {
        dpi = kDefaultDpi;
    }
    return static_cast<float>(dpi) / static_cast<float>(kDefaultDpi);
}

bool key_down(int virtual_key) {
    return (GetKeyState(virtual_key) & 0x8000) != 0;
}

ui::Modifier keyboard_modifiers() {
    ui::Modifier modifiers = ui::Modifier::None;
    if (key_down(VK_SHIFT)) {
        modifiers = modifiers | ui::Modifier::Shift;
    }
    if (key_down(VK_CONTROL)) {
        modifiers = modifiers | ui::Modifier::Control;
    }
    if (key_down(VK_MENU)) {
        modifiers = modifiers | ui::Modifier::Alt;
    }
    if (key_down(VK_LWIN) || key_down(VK_RWIN)) {
        modifiers = modifiers | ui::Modifier::Meta;
    }
    if ((GetKeyState(VK_CAPITAL) & 1) != 0) {
        modifiers = modifiers | ui::Modifier::CapsLock;
    }
    if ((GetKeyState(VK_NUMLOCK) & 1) != 0) {
        modifiers = modifiers | ui::Modifier::NumLock;
    }
    return modifiers;
}

// Mouse messages carry Shift and Control in wParam, which keeps them tied to the message instead of
// to the live keyboard state; Alt and Meta are only available from the keyboard state.
ui::Modifier mouse_modifiers(WPARAM wparam) {
    ui::Modifier modifiers = keyboard_modifiers();
    modifiers = static_cast<ui::Modifier>(
        static_cast<std::uint8_t>(modifiers) &
        ~static_cast<std::uint8_t>(ui::Modifier::Shift | ui::Modifier::Control));
    if ((wparam & MK_SHIFT) != 0) {
        modifiers = modifiers | ui::Modifier::Shift;
    }
    if ((wparam & MK_CONTROL) != 0) {
        modifiers = modifiers | ui::Modifier::Control;
    }
    return modifiers;
}

ui::KeyCode logical_key(WPARAM virtual_key) {
    if ((virtual_key >= 'A' && virtual_key <= 'Z') || (virtual_key >= '0' && virtual_key <= '9')) {
        return static_cast<ui::KeyCode>(virtual_key);
    }
    if (virtual_key >= VK_F1 && virtual_key <= VK_F12) {
        return ui::key::kF1 + static_cast<ui::KeyCode>(virtual_key - VK_F1);
    }
    switch (virtual_key) {
    case VK_BACK:
        return ui::key::kBackspace;
    case VK_TAB:
        return ui::key::kTab;
    case VK_RETURN:
        return ui::key::kEnter;
    case VK_ESCAPE:
        return ui::key::kEscape;
    case VK_SPACE:
        return ui::key::kSpace;
    case VK_INSERT:
        return ui::key::kInsert;
    case VK_DELETE:
        return ui::key::kDelete;
    case VK_HOME:
        return ui::key::kHome;
    case VK_END:
        return ui::key::kEnd;
    case VK_PRIOR:
        return ui::key::kPageUp;
    case VK_NEXT:
        return ui::key::kPageDown;
    case VK_LEFT:
        return ui::key::kArrowLeft;
    case VK_UP:
        return ui::key::kArrowUp;
    case VK_RIGHT:
        return ui::key::kArrowRight;
    case VK_DOWN:
        return ui::key::kArrowDown;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return ui::key::kShift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return ui::key::kControl;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return ui::key::kAlt;
    case VK_LWIN:
    case VK_RWIN:
        return ui::key::kMeta;
    case VK_CAPITAL:
        return ui::key::kCapsLock;
    default:
        return ui::key::kUnknown;
    }
}

std::uint32_t button_bit(ui::PointerButton button) {
    return 1U << static_cast<std::uint32_t>(button);
}

} // namespace

bool Win32InputTranslator::pending() const {
    return queue_ != nullptr && !queue_->empty();
}

void Win32InputTranslator::push(ui::InputEvent event) const {
    if (queue_ != nullptr) {
        event.timestamp_s = steady_now_seconds();
        // A full queue sets its overflow flag; the application reports it.
        (void)queue_->push(event);
    }
}

ui::InputEvent Win32InputTranslator::pointer_event(HWND window, ui::InputEventType type, int x,
                                                   int y, ui::Modifier modifiers) {
    const float scale = dpi_scale_of(window);
    ui::InputEvent event;
    event.type = type;
    event.x = static_cast<float>(x) / scale;
    event.y = static_cast<float>(y) / scale;
    if (has_last_) {
        event.dx = event.x - last_x_;
        event.dy = event.y - last_y_;
    }
    last_x_ = event.x;
    last_y_ = event.y;
    has_last_ = true;
    event.modifiers = modifiers;
    event.is_primary = true;
    event.pressure = buttons_down_ != 0 ? kActivePressure : 0.0f;
    return event;
}

void Win32InputTranslator::pointer_button(HWND window, ui::PointerButton button, bool down,
                                          LPARAM lparam, WPARAM wparam) {
    const std::uint32_t bit = button_bit(button);
    if (down) {
        if (buttons_down_ == 0) {
            // Keep receiving moves and the release even when the pointer leaves the window.
            SetCapture(window);
        }
        buttons_down_ |= bit;
    } else {
        if ((buttons_down_ & bit) == 0) {
            return; // a release for a press this window never saw
        }
        buttons_down_ &= ~bit;
    }
    ui::InputEvent event = pointer_event(
        window, down ? ui::InputEventType::PointerDown : ui::InputEventType::PointerUp,
        GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), mouse_modifiers(wparam));
    event.button = button;
    push(event);
    if (!down && buttons_down_ == 0 && GetCapture() == window) {
        ReleaseCapture();
    }
}

void Win32InputTranslator::pointer_message(HWND window, WPARAM wparam,
                                           win32::PointerMessage message) {
    const std::uint32_t pointer_id = GET_POINTERID_WPARAM(wparam);
    POINTER_INFO info{};
    const bool has_info = GetPointerInfo(pointer_id, &info) != FALSE;
    // A stale pointer id (already gone) has nothing to translate except a cancel, which must still
    // clear the router's capture/active state for that id.
    if (!has_info && message != win32::PointerMessage::Cancel) {
        return;
    }

    win32::PointerSample sample;
    sample.pointer_id = pointer_id;
    sample.message = message;
    // The SDK exposes the PRIMARY message flag; there is no INPRIMARY_RANGE flag on this SDK, so
    // the primary pointer is the one Windows marks primary in the message.
    sample.primary = IS_POINTER_PRIMARY_WPARAM(wparam) != FALSE;
    if (has_info) {
        sample.type = pointer_type_of(info.pointerType);
        sample.in_contact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        // Windows has no WM_POINTERCANCEL message: a canceled sequence is reported through the
        // POINTER_FLAG_CANCELED flag (typically on the up message) and through
        // WM_POINTERCAPTURECHANGED. Both map to ui::InputEventType::PointerCancel.
        if ((info.pointerFlags & POINTER_FLAG_CANCELED) != 0) {
            sample.message = win32::PointerMessage::Cancel;
        }
        POINT point = info.ptPixelLocation;
        if (ScreenToClient(window, &point) != FALSE) {
            sample.client_x = static_cast<float>(point.x);
            sample.client_y = static_cast<float>(point.y);
        }
        if (info.pointerType == PT_PEN) {
            POINTER_PEN_INFO pen_info{};
            if (GetPointerPenInfo(pointer_id, &pen_info) != FALSE) {
                sample.pressure = pen_info.pressure;
            }
        }
    } else {
        sample.type = win32::PointerType::Unknown;
        sample.in_contact = true;
    }
    if (sample.message == win32::PointerMessage::Down ||
        sample.message == win32::PointerMessage::Up) {
        sample.button = ui::PointerButton::Left;
    }

    ui::InputEvent event;
    if (win32::make_pointer_event(sample, dpi_scale_of(window), event)) {
        push(event);
    }
}

void Win32InputTranslator::touch_input(HWND window, WPARAM wparam, LPARAM lparam) {
    const auto handle = reinterpret_cast<HTOUCHINPUT>(lparam);
    const UINT contact_count = LOWORD(wparam);
    if (contact_count != 0) {
        std::array<TOUCHINPUT, kMaxTouchContacts> inputs{};
        const UINT requested = std::min<UINT>(contact_count, static_cast<UINT>(inputs.size()));
        if (GetTouchInputInfo(handle, requested, inputs.data(), sizeof(TOUCHINPUT)) != FALSE) {
            const float scale = dpi_scale_of(window);
            for (UINT i = 0; i < requested; ++i) {
                const TOUCHINPUT& input = inputs[i];
                POINT point{TOUCH_COORD_TO_PIXEL(input.x), TOUCH_COORD_TO_PIXEL(input.y)};
                ScreenToClient(window, &point);
                win32::TouchSample sample;
                sample.touch_id = input.dwID;
                sample.primary = (input.dwFlags & TOUCHEVENTF_PRIMARY) != 0;
                sample.down = (input.dwFlags & TOUCHEVENTF_DOWN) != 0;
                sample.up = (input.dwFlags & TOUCHEVENTF_UP) != 0;
                sample.move = (input.dwFlags & TOUCHEVENTF_MOVE) != 0;
                sample.client_x = static_cast<float>(point.x);
                sample.client_y = static_cast<float>(point.y);
                ui::InputEvent event;
                if (win32::make_touch_event(sample, scale, event)) {
                    push(event);
                }
            }
        }
    }
    CloseTouchInputHandle(handle);
}

void Win32InputTranslator::cancel_pointer(HWND window) {
    if (buttons_down_ == 0) {
        return;
    }
    buttons_down_ = 0;
    ui::InputEvent event;
    event.type = ui::InputEventType::PointerCancel;
    event.x = last_x_;
    event.y = last_y_;
    event.is_primary = true;
    push(event);
    if (GetCapture() == window) {
        ReleaseCapture();
    }
}

bool Win32InputTranslator::handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                  LRESULT& result) {
    result = 0;
    switch (message) {
    // Touch and pen arrive as pointer messages. Handling them here (returning true) also stops
    // Windows from promoting the contact to legacy mouse messages, so the mouse path below stays
    // the only mouse producer.
    case WM_POINTERDOWN:
        pointer_message(window, wparam, win32::PointerMessage::Down);
        return true;
    case WM_POINTERUPDATE:
        pointer_message(window, wparam, win32::PointerMessage::Update);
        return true;
    case WM_POINTERUP:
        pointer_message(window, wparam, win32::PointerMessage::Up);
        return true;
    case WM_POINTERCAPTURECHANGED:
        // Pointer cancellation is surfaced here (there is no WM_POINTERCANCEL message).
        pointer_message(window, wparam, win32::PointerMessage::Cancel);
        return true;
    // Legacy fallback for windows that register for WM_TOUCH instead of pointer promotion.
    case WM_TOUCH:
        touch_input(window, wparam, lparam);
        return true;
    case WM_MOUSEMOVE:
        push(pointer_event(window, ui::InputEventType::PointerMove, GET_X_LPARAM(lparam),
                           GET_Y_LPARAM(lparam), mouse_modifiers(wparam)));
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        pointer_button(window, ui::PointerButton::Left, message == WM_LBUTTONDOWN, lparam, wparam);
        return true;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        pointer_button(window, ui::PointerButton::Right, message == WM_RBUTTONDOWN, lparam, wparam);
        return true;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        pointer_button(window, ui::PointerButton::Middle, message == WM_MBUTTONDOWN, lparam,
                       wparam);
        return true;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        pointer_button(window,
                       GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? ui::PointerButton::Back
                                                              : ui::PointerButton::Forward,
                       message == WM_XBUTTONDOWN, lparam, wparam);
        result = TRUE; // required so the system stops looking for another handler
        return true;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // Wheel coordinates are screen coordinates, unlike every other mouse message.
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        ui::InputEvent event = pointer_event(window, ui::InputEventType::Wheel, point.x, point.y,
                                             mouse_modifiers(GET_KEYSTATE_WPARAM(wparam)));
        // Notches with the web sign: positive dy scrolls content down, positive dx scrolls right.
        const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / kWheelNotch;
        event.dx = message == WM_MOUSEHWHEEL ? notches : 0.0f;
        event.dy = message == WM_MOUSEWHEEL ? -notches : 0.0f;
        push(event);
        return true;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lparam) != window) {
            cancel_pointer(window);
        }
        return true;
    case WM_CANCELMODE:
        cancel_pointer(window);
        return false;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        if (message == WM_KILLFOCUS) {
            cancel_pointer(window);
        }
        ui::InputEvent event;
        event.type = message == WM_SETFOCUS ? ui::InputEventType::Focus : ui::InputEventType::Blur;
        push(event);
        return false; // the window still tracks its own focus state
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        ui::InputEvent event;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        event.type = down ? ui::InputEventType::KeyDown : ui::InputEventType::KeyUp;
        event.key = logical_key(wparam);
        event.modifiers = keyboard_modifiers();
        event.repeat = down && (lparam & (1 << 30)) != 0;
        push(event);
        // System keys (Alt combinations, F10) must still reach DefWindowProc.
        return message == WM_KEYDOWN || message == WM_KEYUP;
    }
    case WM_CHAR: {
        const auto unit = static_cast<wchar_t>(wparam);
        std::uint32_t codepoint = 0;
        if (unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast) {
            high_surrogate_ = unit;
            return true;
        }
        if (unit >= kLowSurrogateFirst && unit <= kLowSurrogateLast) {
            if (high_surrogate_ != 0) {
                codepoint = kSurrogateBase +
                            ((static_cast<std::uint32_t>(high_surrogate_ - kHighSurrogateFirst)
                              << kSurrogateBits) |
                             static_cast<std::uint32_t>(unit - kLowSurrogateFirst));
            }
        } else if (unit >= kFirstPrintable && unit != kDelete) {
            codepoint = unit; // control characters arrive as key events instead
        }
        high_surrogate_ = 0;
        if (codepoint != 0) {
            ui::InputEvent event;
            event.type = ui::InputEventType::Text;
            event.text_codepoint = codepoint;
            event.modifiers = keyboard_modifiers();
            push(event);
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace tessera::hal
