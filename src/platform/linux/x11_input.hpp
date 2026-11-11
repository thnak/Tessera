// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdint>

namespace tessera::hal {

// Translates X11 core pointer, wheel, key and focus events into ui::InputEvent and pushes them into
// the application's queue, in logical pixels. Everything runs on the window's owning thread, so the
// queue's single producer is the UI thread. Text comes from the window's X input context when it
// has one (Xutf8LookupString: dead keys, compose sequences and text committed by an input method
// such as ibus or fcitx), and from the keysym of the pressed key otherwise (ASCII, Latin-1, keypad
// digits, Unicode keysyms). Pre-edit display and candidate windows are not drawn yet.
class X11InputTranslator {
public:
    void set_queue(ui::InputQueue* queue) {
        queue_ = queue;
    }

    // True while the queue holds input the application has not consumed yet; a wait uses this to
    // wake for input, since input never becomes a WindowEvent.
    [[nodiscard]] bool pending() const;

    // The window's input context (XIC), or null for the keysym-only path. The window owns it.
    void set_input_context(XIC context) {
        context_ = context;
    }

    // Returns true when `event` was an input event this translator consumed. `scale` is the
    // window's DPI scale (physical / logical pixels). `auto_repeat` marks a KeyPress that the
    // caller identified as the second half of a release/press auto-repeat pair. `filtered` is
    // XFilterEvent's answer for a key event: the input method consumed it (a dead key or a
    // compose step), so it is still a key event but types nothing by itself.
    bool handle(const XEvent& event, float scale, bool auto_repeat, bool filtered = false);

    // True when `release` and `press` are the release/press pair the X server generates for one
    // auto-repeat tick (same keycode and timestamp).
    [[nodiscard]] static bool is_auto_repeat_pair(const XKeyEvent& release, const XKeyEvent& press);

private:
    void push(ui::InputEvent event) const;
    [[nodiscard]] ui::InputEvent pointer_event(ui::InputEventType type, int x, int y,
                                               unsigned state, float scale);
    void key_event(const XKeyEvent& key, bool down, bool auto_repeat, bool filtered);
    void push_text(std::uint32_t codepoint, ui::Modifier modifiers) const;
    void push_typed_text(const XKeyEvent& key, ui::Modifier modifiers);
    void cancel_pointer();

    ui::InputQueue* queue_ = nullptr;
    XIC context_ = nullptr;
    std::uint32_t buttons_down_ = 0;
    float last_x_ = 0.0F;
    float last_y_ = 0.0F;
    bool has_last_ = false;
};

} // namespace tessera::hal
