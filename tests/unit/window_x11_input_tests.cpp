// SPDX-License-Identifier: MIT
//
// X11 window input under Xvfb: real core events (sent from a second connection with XSendEvent)
// reach the window's ui::InputQueue as normalized pointer, wheel, key and text events.

#include "tessera/hal/window.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::Modifier;
using tessera::ui::PointerButton;

constexpr unsigned long kEventTime = 1000;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_x11_input: " << message << "\n";
    }
    return condition;
}

bool approx(float value, float expected) {
    return value >= expected - 0.5F && value <= expected + 0.5F;
}

::Window find_window(Display* display, ::Window window, std::string_view title) {
    char* name = nullptr;
    if (XFetchName(display, window, &name) != 0 && name != nullptr) {
        const bool matches = title == name;
        XFree(name);
        if (matches) {
            return window;
        }
    }
    ::Window root = 0;
    ::Window parent = 0;
    ::Window* children = nullptr;
    unsigned int child_count = 0;
    if (XQueryTree(display, window, &root, &parent, &children, &child_count) == 0) {
        return 0;
    }
    ::Window found = 0;
    for (unsigned int i = 0; i < child_count && found == 0; ++i) {
        found = find_window(display, children[i], title);
    }
    if (children != nullptr) {
        XFree(children);
    }
    return found;
}

struct Sender {
    Display* display;
    ::Window target;

    void key(int type, KeySym keysym, unsigned state) const {
        XEvent event{};
        event.xkey.type = type;
        event.xkey.display = display;
        event.xkey.window = target;
        event.xkey.root = DefaultRootWindow(display);
        event.xkey.time = kEventTime;
        event.xkey.x = 1;
        event.xkey.y = 1;
        event.xkey.state = state;
        event.xkey.keycode = XKeysymToKeycode(display, keysym);
        event.xkey.same_screen = True;
        XSendEvent(display, target, False, KeyPressMask | KeyReleaseMask, &event);
        XSync(display, False);
    }

    void button(int type, unsigned button, int x, int y, unsigned state = 0) const {
        XEvent event{};
        event.xbutton.type = type;
        event.xbutton.display = display;
        event.xbutton.window = target;
        event.xbutton.root = DefaultRootWindow(display);
        event.xbutton.time = kEventTime;
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.state = state;
        event.xbutton.button = button;
        event.xbutton.same_screen = True;
        XSendEvent(display, target, False, ButtonPressMask | ButtonReleaseMask, &event);
        XSync(display, False);
    }

    void motion(int x, int y, unsigned state = 0) const {
        XEvent event{};
        event.xmotion.type = MotionNotify;
        event.xmotion.display = display;
        event.xmotion.window = target;
        event.xmotion.root = DefaultRootWindow(display);
        event.xmotion.time = kEventTime;
        event.xmotion.x = x;
        event.xmotion.y = y;
        event.xmotion.state = state;
        event.xmotion.same_screen = True;
        XSendEvent(display, target, False, PointerMotionMask, &event);
        XSync(display, False);
    }
};

std::vector<InputEvent> drain(tessera::hal::Window& window, tessera::ui::InputQueue& queue) {
    std::vector<InputEvent> events;
    // The wait wakes while the queue holds input; a short bound keeps an input-less run honest.
    window.wait_for_events(0.5);
    InputEvent event;
    while (queue.pop(event)) {
        events.push_back(event);
    }
    return events;
}

bool pointer_and_wheel(Sender& sender, tessera::hal::Window& window, tessera::ui::InputQueue& queue,
                       float scale) {
    sender.motion(96, 48);
    std::vector<InputEvent> events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::PointerMove,
                "motion becomes one PointerMove") ||
        !expect(approx(events[0].x, 96.0F / scale) && approx(events[0].y, 48.0F / scale),
                "motion coordinates are logical pixels")) {
        return false;
    }

    sender.button(ButtonPress, Button1, 96, 48);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::PointerDown &&
                    events[0].button == PointerButton::Left,
                "button 1 press is a left PointerDown") ||
        !expect(events[0].pressure > 0.0F, "pressed pointer reports pressure")) {
        return false;
    }
    sender.button(ButtonRelease, Button1, 96, 48);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::PointerUp &&
                    events[0].button == PointerButton::Left,
                "button 1 release is a left PointerUp")) {
        return false;
    }

    sender.button(ButtonPress, Button5, 96, 48);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::Wheel &&
                    events[0].dy > 0.0F && events[0].dx == 0.0F,
                "button 5 is a wheel step down with the web sign")) {
        return false;
    }
    sender.button(ButtonRelease, Button5, 96, 48);
    // A wheel notch has no release: nothing may be queued for it.
    return expect(drain(window, queue).empty(), "wheel release produces no event");
}

bool keyboard(Sender& sender, tessera::hal::Window& window, tessera::ui::InputQueue& queue) {
    sender.key(KeyPress, XK_a, 0);
    std::vector<InputEvent> events = drain(window, queue);
    if (!expect(events.size() == 2 && events[0].type == InputEventType::KeyDown &&
                    events[0].key == 'A' && !events[0].repeat,
                "plain a is KeyDown 'A'") ||
        !expect(events[1].type == InputEventType::Text && events[1].text_codepoint == 'a',
                "plain a types 'a'")) {
        return false;
    }
    sender.key(KeyRelease, XK_a, 0);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::KeyUp &&
                    events[0].key == 'A',
                "release is KeyUp 'A'")) {
        return false;
    }

    sender.key(KeyPress, XK_a, ShiftMask);
    events = drain(window, queue);
    if (!expect(events.size() == 2 && events[1].type == InputEventType::Text &&
                    events[1].text_codepoint == 'A' &&
                    tessera::ui::has_modifier(events[0].modifiers, Modifier::Shift),
                "shift+a types 'A' with the Shift modifier")) {
        return false;
    }

    sender.key(KeyPress, XK_a, ControlMask);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].type == InputEventType::KeyDown &&
                    tessera::ui::has_modifier(events[0].modifiers, Modifier::Control),
                "ctrl+a is a key event with no text")) {
        return false;
    }

    sender.key(KeyPress, XK_Left, 0);
    events = drain(window, queue);
    if (!expect(events.size() == 1 && events[0].key == tessera::ui::key::kArrowLeft,
                "left arrow maps to kArrowLeft and types nothing")) {
        return false;
    }

    // Auto-repeat: a release immediately followed by a press with the same keycode and time is
    // one repeat KeyDown, not a KeyUp plus a fresh KeyDown.
    sender.key(KeyRelease, XK_b, 0);
    sender.key(KeyPress, XK_b, 0);
    events = drain(window, queue);
    return expect(events.size() == 2 && events[0].type == InputEventType::KeyDown &&
                      events[0].repeat && events[1].type == InputEventType::Text,
                  "release/press pair becomes one repeat KeyDown");
}

} // namespace

int main() {
    const char* display_name = std::getenv("DISPLAY");
    if (display_name == nullptr || display_name[0] == '\0') {
        std::cout << "window_x11_input: skipped (DISPLAY is not set)\n";
        return 0;
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cout << "window_x11_input: skipped (cannot open DISPLAY)\n";
        return 0;
    }

    tessera::hal::WindowDesc desc;
    desc.title = "Tessera X11 input test " + std::to_string(static_cast<unsigned long>(getpid()));
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;
    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        XCloseDisplay(display);
        return 1;
    }
    const ::Window native = find_window(display, DefaultRootWindow(display), desc.title);
    if (!expect(native != 0, "native window exists")) {
        XCloseDisplay(display);
        return 2;
    }

    tessera::ui::InputQueue queue(64);
    window->set_input_queue(&queue);
    Sender sender{display, native};
    const float scale = window->metrics().dpi_scale;

    const bool ok =
        pointer_and_wheel(sender, *window, queue, scale) && keyboard(sender, *window, queue);

    window->set_input_queue(nullptr);
    window.reset();
    XCloseDisplay(display);
    if (!ok) {
        return 3;
    }
    std::cout << "window_x11_input: ok\n";
    return 0;
}
