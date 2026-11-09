// SPDX-License-Identifier: MIT

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "tessera/ui/input.hpp"
#include "win32_pointer_translation.hpp"

#include <cstdint>
#include <windows.h>

namespace tessera::hal {

// Translates Win32 mouse, touch/pen (WM_POINTER*, WM_TOUCH fallback), wheel, keyboard and
// character messages into ui::InputEvent and pushes them into the application's queue. No Win32
// type crosses into ui/. Everything runs on the window's owning thread, so the queue's single
// producer is the UI thread.
class Win32InputTranslator {
public:
    void set_queue(ui::InputQueue* queue) {
        queue_ = queue;
    }

    // True while the queue holds input the application has not consumed yet. A wait uses this to
    // wake for input, since input never becomes a WindowEvent.
    [[nodiscard]] bool pending() const;

    // Returns true when the message was fully handled and `result` is the window-procedure result.
    // Messages that are recorded but must still reach the default procedure (system keys, focus)
    // return false.
    bool handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

private:
    void push(ui::InputEvent event) const;
    [[nodiscard]] ui::InputEvent pointer_event(HWND window, ui::InputEventType type, int x, int y,
                                               ui::Modifier modifiers);
    void pointer_button(HWND window, ui::PointerButton button, bool down, LPARAM lparam,
                        WPARAM wparam);
    void pointer_message(HWND window, WPARAM wparam, win32::PointerMessage message);
    void touch_input(HWND window, WPARAM wparam, LPARAM lparam);
    void cancel_pointer(HWND window);

    ui::InputQueue* queue_ = nullptr;
    std::uint32_t buttons_down_ = 0;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    bool has_last_ = false;
    wchar_t high_surrogate_ = 0;
};

} // namespace tessera::hal
