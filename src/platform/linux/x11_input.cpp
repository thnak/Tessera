// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "x11_input.hpp"
#include "x11_input_translation.hpp"

namespace tessera::hal {
namespace {

constexpr float kActivePressure = 0.5F;
constexpr unsigned kControlAltMeta = x11::kControlMask | x11::kMod1Mask | x11::kMod4Mask;
constexpr std::uint32_t kFirstPrintable = 0x20;
constexpr std::uint32_t kDeleteCharacter = 0x7F;

std::uint32_t button_bit(ui::PointerButton button) {
    return 1U << static_cast<std::uint32_t>(button);
}

} // namespace

bool X11InputTranslator::pending() const {
    return queue_ != nullptr && !queue_->empty();
}

void X11InputTranslator::push(ui::InputEvent event) const {
    if (queue_ != nullptr) {
        event.timestamp_s = steady_now_seconds();
        // A full queue sets its overflow flag; the application reports it.
        (void)queue_->push(event);
    }
}

ui::InputEvent X11InputTranslator::pointer_event(ui::InputEventType type, int x, int y,
                                                 unsigned state, float scale) {
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
    event.modifiers = x11::modifiers_from_state(state);
    event.is_primary = true;
    event.pressure = buttons_down_ != 0 ? kActivePressure : 0.0F;
    return event;
}

void X11InputTranslator::cancel_pointer() {
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
}

void X11InputTranslator::key_event(const XKeyEvent& key, bool down, bool auto_repeat) {
    // The unmodified keysym is the layout-level key; XLookupString applies the modifiers to give
    // the keysym that is typed.
    XKeyEvent copy = key;
    const KeySym base = XLookupKeysym(&copy, 0);

    ui::InputEvent event;
    event.type = down ? ui::InputEventType::KeyDown : ui::InputEventType::KeyUp;
    event.key = x11::logical_key_from_keysym(base);
    event.modifiers = x11::modifiers_from_state(key.state);
    event.repeat = down && auto_repeat;
    push(event);

    if (!down || (key.state & kControlAltMeta) != 0) {
        return; // shortcuts are key events, not text
    }
    char buffer[8];
    KeySym typed = NoSymbol;
    XLookupString(&copy, buffer, static_cast<int>(sizeof(buffer)), &typed, nullptr);
    const std::uint32_t codepoint = x11::codepoint_from_keysym(typed);
    if (codepoint < kFirstPrintable || codepoint == kDeleteCharacter) {
        return;
    }
    ui::InputEvent text;
    text.type = ui::InputEventType::Text;
    text.text_codepoint = codepoint;
    text.modifiers = event.modifiers;
    push(text);
}

bool X11InputTranslator::is_auto_repeat_pair(const XKeyEvent& release, const XKeyEvent& press) {
    return release.keycode == press.keycode && release.time == press.time;
}

bool X11InputTranslator::handle(const XEvent& event, float scale, bool auto_repeat) {
    switch (event.type) {
    case KeyPress:
        key_event(event.xkey, true, auto_repeat);
        return true;
    case KeyRelease:
        key_event(event.xkey, false, false);
        return true;
    case MotionNotify:
        push(pointer_event(ui::InputEventType::PointerMove, event.xmotion.x, event.xmotion.y,
                           event.xmotion.state, scale));
        return true;
    case ButtonPress: {
        const XButtonEvent& button = event.xbutton;
        const x11::ButtonAction action = x11::button_action(button.button);
        if (action.kind == x11::ButtonAction::Kind::Wheel) {
            ui::InputEvent wheel =
                pointer_event(ui::InputEventType::Wheel, button.x, button.y, button.state, scale);
            wheel.dx = action.dx;
            wheel.dy = action.dy;
            push(wheel);
        } else if (action.kind == x11::ButtonAction::Kind::Button) {
            buttons_down_ |= button_bit(action.button);
            ui::InputEvent down = pointer_event(ui::InputEventType::PointerDown, button.x, button.y,
                                                button.state, scale);
            down.button = action.button;
            push(down);
        }
        return true;
    }
    case ButtonRelease: {
        const XButtonEvent& button = event.xbutton;
        const x11::ButtonAction action = x11::button_action(button.button);
        if (action.kind == x11::ButtonAction::Kind::Button) {
            const std::uint32_t bit = button_bit(action.button);
            if ((buttons_down_ & bit) == 0) {
                return true; // a release for a press this window never saw
            }
            ui::InputEvent up = pointer_event(ui::InputEventType::PointerUp, button.x, button.y,
                                              button.state, scale);
            buttons_down_ &= ~bit;
            up.button = action.button;
            up.pressure = 0.0F;
            push(up);
        }
        return true;
    }
    case FocusIn: {
        ui::InputEvent focus;
        focus.type = ui::InputEventType::Focus;
        push(focus);
        return false; // the window still tracks its own focus state
    }
    case FocusOut: {
        cancel_pointer();
        ui::InputEvent blur;
        blur.type = ui::InputEventType::Blur;
        push(blur);
        return false;
    }
    default:
        return false;
    }
}

} // namespace tessera::hal
