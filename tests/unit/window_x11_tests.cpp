// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#include <X11/Xlib.h>
// X.h defines `CursorShape` as an object-like macro, which would rewrite the HAL enumerator names
// used below into `0`; drop it, exactly as window_cursor_x11.hpp does.
#ifdef CursorShape
#undef CursorShape
#endif
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_x11: " << message << "\n";
    }
    return condition;
}

bool approx(float value, float expected, float tolerance = 1.0f) {
    return value >= expected - tolerance && value <= expected + tolerance;
}

// The HAL hides the native handle by design, so the test finds the window the way an external
// client (window manager, input method) would: walk the root tree and match WM_NAME.
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

bool contains_event(const std::array<tessera::hal::WindowEvent, 32>& events, std::size_t count,
                    tessera::hal::WindowEventType type) {
    for (std::size_t i = 0; i < count; ++i) {
        if (events[i].type == type) {
            return true;
        }
    }
    return false;
}

// Posts WM_DELETE_WINDOW from a separate connection after a delay, so the owning thread is
// already parked in wait_for_events when the message reaches the server.
std::thread post_close_soon(::Window target) {
    return std::thread([target]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return;
        }
        const Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
        const Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XClientMessageEvent message{};
        message.type = ClientMessage;
        message.display = display;
        message.window = target;
        message.message_type = protocols;
        message.format = 32;
        message.data.l[0] = static_cast<long>(delete_window);
        message.data.l[1] = CurrentTime;
        XEvent event{};
        event.xclient = message;
        (void)XSendEvent(display, target, False, NoEventMask, &event);
        XFlush(display);
        XCloseDisplay(display);
    });
}

} // namespace

int main() {
    using tessera::hal::WindowEvent;
    using tessera::hal::WindowEventType;
    using tessera::hal::WindowMetrics;

    const char* display_name = std::getenv("DISPLAY");
    if (display_name == nullptr || display_name[0] == '\0') {
        std::cout << "window_x11: skipped (DISPLAY is not set)\n";
        return 0;
    }
    // Pin the X11 backend: on a Wayland desktop the Linux selector would create a Wayland window
    // (issue #155), and this test proves the X11 backend specifically.
    setenv("TESSERA_WINDOW_BACKEND", "x11", 1);
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cout << "window_x11: skipped (cannot open DISPLAY)\n";
        return 0;
    }

    const std::string suffix =
        std::to_string(CurrentTime) + "-" + std::to_string(static_cast<unsigned long>(getpid()));
    tessera::hal::WindowDesc desc;
    desc.title = "Tessera X11 window test " + suffix;
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

    const tessera::hal::WindowMetrics initial = window->metrics();
    if (!expect(initial.physical_width > 0 && initial.physical_height > 0,
                "initial physical size is non-zero") ||
        !expect(approx(static_cast<float>(initial.logical_width), 320.0f),
                "initial logical width follows WindowDesc") ||
        !expect(approx(static_cast<float>(initial.logical_height), 200.0f),
                "initial logical height follows WindowDesc") ||
        !expect(initial.dpi_scale > 0.0f, "initial dpi scale is positive") ||
        !expect(!initial.focused, "hidden window is not focused") ||
        !expect(!initial.minimized && !initial.occluded,
                "X11 reports no minimize/occlusion signal")) {
        XCloseDisplay(display);
        return 3;
    }

    std::array<WindowEvent, 32> events{};
    if (!expect(window->poll_events(events) == 0, "creation records no events")) {
        XCloseDisplay(display);
        return 4;
    }

    // RandR reports the active CRTC mode when the extension is present, and "unknown" when it is
    // absent or the mode has no usable timing (Xvfb's synthetic modes report a zero dot clock).
    // Both are valid answers; the query must never invent a rate or throw.
    const double refresh_hz = window->display_refresh_hz();
    if (!expect(refresh_hz == tessera::hal::kUnknownRefreshHz ||
                    (refresh_hz >= tessera::hal::kMinDisplayRefreshHz &&
                     refresh_hz <= tessera::hal::kMaxDisplayRefreshHz),
                "display refresh is unknown or a sane rate")) {
        XCloseDisplay(display);
        return 15;
    }
    std::cout << "window_x11: display refresh " << refresh_hz << " Hz\n";

    // Resizing the hidden window from an external client produces a ConfigureNotify that the HAL
    // normalizes into one coalesced Resize event carrying the new metrics.
    XResizeWindow(display, native, 480, 320);
    XSync(display, False);
    if (!expect(window->wait_for_events(1.0), "configure wakes the wait")) {
        XCloseDisplay(display);
        return 5;
    }
    const std::size_t resize_count = window->poll_events(events);
    const tessera::hal::WindowMetrics resized = window->metrics();
    if (!expect(contains_event(events, resize_count, WindowEventType::Resize),
                "resize produces a Resize event") ||
        !expect(resized.physical_width == 480 && resized.physical_height == 320,
                "metrics follow the new size") ||
        !expect(approx(resized.dpi_scale, initial.dpi_scale), "resize keeps the dpi scale")) {
        XCloseDisplay(display);
        return 6;
    }

    // An idle window times out instead of spinning, and a past/negative timeout degrades to a
    // non-blocking poll.
    double start = tessera::hal::steady_now_seconds();
    const bool has_event = window->wait_for_events(0.03);
    double elapsed = tessera::hal::steady_now_seconds() - start;
    if (!expect(!has_event, "idle wait reports no event") ||
        !expect(elapsed >= 0.02, "idle wait blocks for the timeout") ||
        !expect(elapsed < 2.0, "idle wait is bounded")) {
        XCloseDisplay(display);
        return 7;
    }
    start = tessera::hal::steady_now_seconds();
    if (!expect(!window->wait_for_events(-1.0), "negative timeout reports no event") ||
        !expect(tessera::hal::steady_now_seconds() - start < 0.5,
                "negative timeout returns immediately")) {
        XCloseDisplay(display);
        return 8;
    }

    // A ClientMessage from another connection wakes a parked wait and becomes Close.
    desc.title = "Tessera X11 close test " + suffix;
    auto external_window = tessera::hal::create_window(desc);
    if (!expect(external_window != nullptr, "create_window returns a second window")) {
        XCloseDisplay(display);
        return 9;
    }
    const ::Window external_native = find_window(display, DefaultRootWindow(display), desc.title);
    if (!expect(external_native != 0, "second native window exists")) {
        XCloseDisplay(display);
        return 10;
    }
    std::thread poster = post_close_soon(external_native);
    start = tessera::hal::steady_now_seconds();
    const bool woke = external_window->wait_for_events(2.0);
    elapsed = tessera::hal::steady_now_seconds() - start;
    poster.join();
    std::size_t count = external_window->poll_events(events);
    if (!expect(woke, "posted ClientMessage wakes the wait") ||
        !expect(elapsed < 2.0, "posted ClientMessage wakes quickly") ||
        !expect(contains_event(events, count, WindowEventType::Close),
                "ClientMessage normalizes to Close")) {
        XCloseDisplay(display);
        return 11;
    }
    external_window.reset();

    // Window::close() sends the same protocol message and wakes the owning thread's wait.
    window->close();
    count = window->wait_for_events(1.0) ? window->poll_events(events) : 0;
    if (!expect(count > 0 && contains_event(events, count, WindowEventType::Close),
                "close() produces a Close event")) {
        XCloseDisplay(display);
        return 12;
    }
    window->close();
    (void)window->wait_for_events(0.0);
    if (!expect(window->poll_events(events) == 0, "second close is a no-op")) {
        XCloseDisplay(display);
        return 13;
    }
    // Full-screen (#232): the X11 backend asks the window manager through EWMH and reports the
    // state the server resolved. Under Xvfb there is no window manager, so the documented answers
    // are "unsupported" and a request that leaves the mode alone; where a WM exists the mode
    // round-trips. Both branches are asserted so the test is meaningful on a bare Xvfb and on a
    // desktop session.
    {
        const bool supported = window->supports_fullscreen();
        const WindowMetrics before_fullscreen = window->metrics();
        window->set_fullscreen(true);
        (void)window->wait_for_events(0.1);
        (void)window->poll_events(events);
        const WindowMetrics after_enter = window->metrics();
        if (supported) {
            if (!expect(after_enter.fullscreen, "a supported enter reports full-screen")) {
                XCloseDisplay(display);
                return 15;
            }
        } else if (!expect(!after_enter.fullscreen,
                           "an unsupported platform stays windowed after a request")) {
            XCloseDisplay(display);
            return 25;
        }
        window->set_fullscreen(false);
        (void)window->wait_for_events(0.1);
        (void)window->poll_events(events);
        const WindowMetrics after_exit = window->metrics();
        if (!expect(!after_exit.fullscreen, "leaving reports the windowed mode") ||
            !expect(after_exit.physical_width == before_fullscreen.physical_width,
                    "leaving restores the pre-full-screen client width") ||
            !expect(after_exit.physical_height == before_fullscreen.physical_height,
                    "leaving restores the pre-full-screen client height")) {
            XCloseDisplay(display);
            return 16;
        }
        // A repeated request is harmless whether or not the platform supports it.
        window->set_fullscreen(true);
        window->set_fullscreen(true);
        window->set_fullscreen(false);
        (void)window->wait_for_events(0.1);
        (void)window->poll_events(events);
    }

    // Caption regions (#232): stored and reported through the metrics. X11 client-side decoration
    // is deferred, so publishing them must not change the platform's own chrome or hit test.
    {
        const std::array<tessera::hal::CaptionRegion, 2> regions{
            tessera::hal::CaptionRegion{tessera::hal::WindowRect{0, 0, 32, 32},
                                        tessera::hal::CaptionRegionKind::SystemMenu},
            tessera::hal::CaptionRegion{tessera::hal::WindowRect{260, 0, 46, 32},
                                        tessera::hal::CaptionRegionKind::Close},
        };
        window->set_caption_regions(regions);
        const WindowMetrics custom = window->metrics();
        if (!expect(custom.caption_region_count == 2, "the caption regions are published") ||
            !expect(custom.caption_regions[0].kind == tessera::hal::CaptionRegionKind::SystemMenu,
                    "the first region keeps its order and kind")) {
            XCloseDisplay(display);
            return 17;
        }
        window->set_caption_regions({});
        if (!expect(window->metrics().caption_region_count == 0,
                    "an empty list clears the published regions")) {
            XCloseDisplay(display);
            return 18;
        }
    }

    // Pointer shape (#191, window-chrome-v1.md section 8.5): the X11 backend creates a font cursor
    // and installs it with XDefineCursor, and reports the resolved shape. A real X server is
    // needed, which is why this half is Linux-only.
    {
        if (!expect(window->supports_cursor(), "the X11 backend supports pointer shapes")) {
            XCloseDisplay(display);
            return 19;
        }
        if (!expect(window->metrics().cursor == tessera::hal::CursorShape::Default,
                    "a fresh window reports the default arrow")) {
            XCloseDisplay(display);
            return 20;
        }
        window->set_cursor(tessera::hal::CursorShape::Pointer);
        XSync(display, False);
        if (!expect(window->metrics().cursor == tessera::hal::CursorShape::Pointer,
                    "a pointer request is reported as the resolved shape")) {
            XCloseDisplay(display);
            return 21;
        }
        window->set_cursor(tessera::hal::CursorShape::Text);
        XSync(display, False);
        if (!expect(window->metrics().cursor == tessera::hal::CursorShape::Text,
                    "a later shape overrides the pointer")) {
            XCloseDisplay(display);
            return 22;
        }
        // CSS `cursor: none` is the hidden shape: XDefineCursor(display, window, None).
        window->set_cursor(tessera::hal::CursorShape::Hidden);
        XSync(display, False);
        if (!expect(window->metrics().cursor == tessera::hal::CursorShape::Hidden,
                    "the hidden shape is reported")) {
            XCloseDisplay(display);
            return 23;
        }
        window->set_cursor(tessera::hal::CursorShape::Default);
        XSync(display, False);
        if (!expect(window->metrics().cursor == tessera::hal::CursorShape::Default,
                    "returning to the default arrow is reported")) {
            XCloseDisplay(display);
            return 24;
        }
    }

    window.reset();

    // Invalid descriptors are rejected without creating a native window.
    tessera::hal::WindowDesc invalid = desc;
    invalid.width = 0;
    if (!expect(tessera::hal::create_window(invalid) == nullptr, "zero width is rejected")) {
        XCloseDisplay(display);
        return 14;
    }

    XCloseDisplay(display);
    std::cout << "window_x11: ok\n";
    return 0;
}
