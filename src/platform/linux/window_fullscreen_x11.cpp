// SPDX-License-Identifier: MIT
//
// X11 full-screen for the HAL window (issue #232).
//
// On X11 the window manager owns the state, not the client: a client asks by sending the EWMH
// `_NET_WM_STATE` ClientMessage and the server answers asynchronously with a `PropertyNotify` and a
// `ConfigureNotify`. The rules that follow from that:
//
//   * Never assume the request succeeded. `metrics().fullscreen` is read back from `_NET_WM_STATE`,
//     so an F11 the window manager handled itself, a tiling WM that refuses, and a user who toggled
//     the state from a taskbar all report the truth.
//   * Never wait for the answer. The round trip is not blocking here: the state is simply re-read
//   on
//     the next metrics call and the property change arrives through the normal event pump.
//   * The client rectangle is remembered as a fallback. A WM with EWMH restores its own geometry;
//     without one (bare Xvfb) the backend moves the window back itself.

#include "window_fullscreen_x11.hpp"

#include <X11/Xatom.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace tessera::hal::detail {
namespace {

Atom intern(Display* display, const char* name) noexcept {
    return display != nullptr ? XInternAtom(display, name, False) : None;
}

// Reads one window property as a list of atoms. `out` must hold at most `capacity` (16 is far more
// than the number of states a window carries). Returns the count, 0 when the property is absent or
// of another type; never allocates more than Xlib returns and always frees it.
std::size_t read_atoms(Display* display, ::Window window, Atom property, Atom type, Atom* out,
                       std::size_t capacity) noexcept {
    if (display == nullptr || property == None || capacity == 0) {
        return 0;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, 32, False, type, &actual_type,
                           &actual_format, &count, &remaining, &data) != Success) {
        return 0;
    }
    std::size_t written = 0;
    if (data != nullptr && actual_type == type && actual_format == 32) {
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long index = 0; index < count && written < capacity; ++index) {
            out[written++] = atoms[index];
        }
    }
    if (data != nullptr) {
        XFree(data);
    }
    return written;
}

bool contains(const Atom* atoms, std::size_t count, Atom wanted) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (atoms[index] == wanted) {
            return true;
        }
    }
    return false;
}

} // namespace

bool x11_fullscreen_init(Display* display, X11FullscreenState& state) noexcept {
    state.net_wm_state = intern(display, "_NET_WM_STATE");
    state.net_wm_state_fullscreen = intern(display, "_NET_WM_STATE_FULLSCREEN");
    return state.net_wm_state != None && state.net_wm_state_fullscreen != None;
}

bool x11_fullscreen_supported(Display* display, ::Window root,
                              const X11FullscreenState& state) noexcept {
    if (display == nullptr || state.net_wm_state == None || state.net_wm_state_fullscreen == None) {
        return false;
    }
    // _NET_SUPPORTED advertising the state atom is the documented "an EWMH window manager is
    // running" signal. A bare Xvfb has no such property, so the request would be silently dropped;
    // answering false here is what turns that into one diagnostic instead of a dead button.
    const Atom net_supported = intern(display, "_NET_SUPPORTED");
    Atom supported[64];
    const std::size_t count =
        read_atoms(display, root, net_supported, XA_ATOM, supported, std::size(supported));
    return contains(supported, count, state.net_wm_state);
}

void x11_fullscreen_remember(Display* display, ::Window window,
                             X11FullscreenState& state) noexcept {
    if (display == nullptr || window == 0) {
        return;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        return;
    }
    Window child = 0;
    int x = 0;
    int y = 0;
    if (XTranslateCoordinates(display, window, attributes.root, 0, 0, &x, &y, &child) == 0) {
        x = 0;
        y = 0;
    }
    const WindowRect bounds{static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(attributes.width),
                            static_cast<float>(attributes.height)};
    // Only capture while the window is not already full-screen: a repeated enter would otherwise
    // remember the full-screen rectangle and restore into it. The active flag follows what
    // `_NET_WM_STATE` says, so the state machine never drifts from the window manager.
    const bool already = x11_fullscreen_active(display, window, state);
    if (already) {
        return;
    }
    (void)fullscreen_enter(state.state, bounds);
    state.restore_bounds = bounds;
    state.restore_x = x;
    state.restore_y = y;
    state.restore_width = attributes.width;
    state.restore_height = attributes.height;
    state.restore_valid = true;
}

void x11_fullscreen_request(Display* display, ::Window window, X11FullscreenState& state,
                            bool fullscreen) noexcept {
    if (display == nullptr || window == 0 || state.net_wm_state == None ||
        state.net_wm_state_fullscreen == None) {
        return;
    }
    // EWMH spells a state change as a ClientMessage to the root window with _NET_WM_STATE as the
    // type: action 1 is add, 0 is remove, and the second data word names the state to change.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = state.net_wm_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = fullscreen ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(state.net_wm_state_fullscreen);
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 2; // source indication: a normal application
    XWindowAttributes attributes{};
    const ::Window root = XGetWindowAttributes(display, window, &attributes) != 0
                              ? attributes.root
                              : DefaultRootWindow(display);
    XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

void x11_fullscreen_restore(Display* display, ::Window window, X11FullscreenState& state) noexcept {
    if (display == nullptr || window == 0 || !state.restore_valid) {
        return;
    }
    // A window manager that honored the request restored its own frame; only a display without one
    // (bare Xvfb) needs the client moved back, and then the remembered rectangle is the only thing
    // that knows where the window was. Moving unconditionally would fight a real WM's placement.
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        return;
    }
    Window child = 0;
    int x = 0;
    int y = 0;
    if (XTranslateCoordinates(display, window, attributes.root, 0, 0, &x, &y, &child) != 0 &&
        x == state.restore_x && y == state.restore_y && attributes.width == state.restore_width &&
        attributes.height == state.restore_height) {
        // Already back at the remembered geometry: nothing to do, and no ConfigureNotify churn.
        state.restore_valid = false;
        return;
    }
    XMoveResizeWindow(display, window, state.restore_x, state.restore_y,
                      static_cast<unsigned int>(std::max(state.restore_width, 1)),
                      static_cast<unsigned int>(std::max(state.restore_height, 1)));
    XFlush(display);
    state.restore_valid = false;
}

bool x11_fullscreen_active(Display* display, ::Window window,
                           const X11FullscreenState& state) noexcept {
    if (display == nullptr || window == 0 || state.net_wm_state == None ||
        state.net_wm_state_fullscreen == None) {
        return false;
    }
    Atom states[16];
    const std::size_t count =
        read_atoms(display, window, state.net_wm_state, XA_ATOM, states, std::size(states));
    return contains(states, count, state.net_wm_state_fullscreen);
}

} // namespace tessera::hal::detail
