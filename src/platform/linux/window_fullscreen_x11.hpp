// SPDX-License-Identifier: MIT

#pragma once

// X11 full-screen support for the HAL window (issue #232, docs/specs/window-chrome-v1.md section
// 13). Lives in its own translation unit because src/platform/linux/window.cpp is at the file-size
// limit.
//
// Xlib's `Window` (an XID) and `tessera::hal::Window` (the platform seam class) share a name; every
// XID here is spelled `::Window` so there is no ambiguity in either direction.

#include "tessera/hal/fullscreen.hpp"

#include <X11/Xlib.h>

namespace tessera::hal::detail {

// EWMH full-screen support for one X11 window. The window manager owns the state and the restore,
// so this records what it needs to ask and reports what the server says, never what was requested.
struct X11FullscreenState {
    FullscreenState state;
    // _NET_WM_STATE and _NET_WM_STATE_FULLSCREEN; None when the atoms are unavailable, which is the
    // "no EWMH window manager" answer (bare Xvfb, a WM without EWMH).
    Atom net_wm_state = None;
    Atom net_wm_state_fullscreen = None;
    // The client rectangle to restore on exit. X11 without a frame-extents query cannot see its
    // decorations, so this is the client rectangle the window had; a real window manager restores
    // its own frame around it.
    WindowRect restore_bounds;
    int restore_x = 0;
    int restore_y = 0;
    int restore_width = 0;
    int restore_height = 0;
    bool restore_valid = false;
};

// Resolves the EWMH atoms once at creation. False means full-screen cannot be requested here (no
// EWMH on this display), which the caller reports as "unsupported" rather than pretending.
[[nodiscard]] bool x11_fullscreen_init(Display* display, X11FullscreenState& state) noexcept;

// True when the request can be made at all: the atoms resolved and the window manager advertises
// _NET_WM_STATE support (`_NET_SUPPORTED`), which is the only reliable "a WM is running" signal.
[[nodiscard]] bool x11_fullscreen_supported(Display* display, ::Window root,
                                            const X11FullscreenState& state) noexcept;

// The client rectangle the window had before entering, captured for the exit path. Called by the
// platform before the state change; a repeated enter keeps the first capture.
void x11_fullscreen_remember(Display* display, ::Window window, X11FullscreenState& state) noexcept;

// Asks the window manager to add or remove _NET_WM_STATE_FULLSCREEN. The change is asynchronous
// (the WM answers with a PropertyNotify plus the new ConfigureNotify), so the metrics report the
// resolved state from _NET_WM_STATE and never this function's return value. Never blocks or waits.
void x11_fullscreen_request(Display* display, ::Window window, X11FullscreenState& state,
                            bool fullscreen) noexcept;

// Puts the client back at the remembered rectangle after the state change, for a display with no
// EWMH window manager (bare Xvfb), where nothing else restores it. A WM that honored the EWMH
// request has already restored its own geometry, so this is a no-op when the server reports the
// window back at the remembered position. Never blocks.
void x11_fullscreen_restore(Display* display, ::Window window, X11FullscreenState& state) noexcept;

// Reads _NET_WM_STATE and returns whether the window is full-screen right now. This is the truth
// the metrics report: a WM shortcut or another client changing the state is observed, not assumed.
[[nodiscard]] bool x11_fullscreen_active(Display* display, ::Window window,
                                         const X11FullscreenState& state) noexcept;

} // namespace tessera::hal::detail
