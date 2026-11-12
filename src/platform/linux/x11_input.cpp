// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "x11_input.hpp"
#include "x11_input_translation.hpp"

#include <cstddef>
#include <string>
#include <string_view>

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

void X11InputTranslator::push_text(std::uint32_t codepoint, ui::Modifier modifiers) const {
    if (codepoint < kFirstPrintable || codepoint == kDeleteCharacter) {
        return; // control characters arrive as key events instead
    }
    ui::InputEvent text;
    text.type = ui::InputEventType::Text;
    text.text_codepoint = codepoint;
    text.modifiers = modifiers;
    push(text);
}

void X11InputTranslator::push_typed_text(const XKeyEvent& key, ui::Modifier modifiers,
                                         bool ime_commit) {
    XKeyEvent copy = key;
    KeySym typed = NoSymbol;
    if (context_ == nullptr) {
        char buffer[8];
        XLookupString(&copy, buffer, static_cast<int>(sizeof(buffer)), &typed, nullptr);
        push_text(x11::codepoint_from_keysym(typed), modifiers);
        return;
    }
    // The input context applies the layout, dead keys, compose sequences and any text an input
    // method committed. Committed text can be long, so an overflow retries with exact room.
    char stack[64];
    std::string heap;
    const char* data = stack;
    Status status = XLookupNone;
    int length =
        Xutf8LookupString(context_, &copy, stack, static_cast<int>(sizeof(stack)), &typed, &status);
    if (status == XBufferOverflow && length > 0) {
        heap.resize(static_cast<std::size_t>(length));
        length = Xutf8LookupString(context_, &copy, heap.data(), length, &typed, &status);
        data = heap.data();
    }
    if (status == XLookupChars || status == XLookupBoth) {
        const std::string_view text(data, length > 0 ? static_cast<std::size_t>(length) : 0U);
        // The pre-edit state machine may already have committed this string as one EditText event;
        // it reports that by consuming the gate, so the text is not inserted twice.
        if (ime_commit && ime_gate_.consume != nullptr &&
            ime_gate_.consume(ime_gate_.context, text)) {
            return;
        }
        std::size_t position = 0;
        while (position < text.size()) {
            push_text(x11::next_utf8_codepoint(text, position), modifiers);
        }
        return;
    }
    if (status == XLookupKeySym) {
        push_text(x11::codepoint_from_keysym(typed), modifiers);
    }
}

void X11InputTranslator::key_event(const XKeyEvent& key, bool down, bool auto_repeat,
                                   bool filtered) {
    // An input method delivers committed text as a KeyPress with keycode 0: text without a key.
    const bool commit = key.keycode == 0;
    const ui::Modifier modifiers = x11::modifiers_from_state(key.state);
    if (!commit) {
        // The unmodified keysym is the layout-level key; the input context (or XLookupString)
        // applies the modifiers to give what is typed.
        XKeyEvent copy = key;
        const KeySym base = XLookupKeysym(&copy, 0);

        ui::InputEvent event;
        event.type = down ? ui::InputEventType::KeyDown : ui::InputEventType::KeyUp;
        event.key = x11::logical_key_from_keysym(base);
        event.modifiers = modifiers;
        event.repeat = down && auto_repeat;
        push(event);
    }
    // A filtered key (a dead key or a compose step) was consumed by the input method: the key
    // event above still happened, but the character arrives with the key that completes it.
    if (!down || filtered || (key.state & kControlAltMeta) != 0) {
        return; // shortcuts are key events, not text
    }
    push_typed_text(key, modifiers, commit);
}

bool X11InputTranslator::is_auto_repeat_pair(const XKeyEvent& release, const XKeyEvent& press) {
    return release.keycode == press.keycode && release.time == press.time;
}

bool X11InputTranslator::handle(const XEvent& event, float scale, bool auto_repeat, bool filtered) {
    switch (event.type) {
    case KeyPress:
        key_event(event.xkey, true, auto_repeat, filtered);
        return true;
    case KeyRelease:
        key_event(event.xkey, false, false, filtered);
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
