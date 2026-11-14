// SPDX-License-Identifier: MIT

#pragma once

// Win32 full-screen support for the HAL window (issue #232, docs/specs/window-chrome-v1.md
// section 13). Lives in its own translation unit because `window.cpp` is already at the file-size
// limit; the window class holds one state value and calls these functions.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "tessera/hal/fullscreen.hpp"

#include <windows.h>

namespace tessera::hal::detail {

// Everything one window needs to leave full-screen exactly as it entered. Owned by the window, not
// a process table, so several windows can be full-screen independently (issue #228).
struct Win32FullscreenState {
    FullscreenState state;
    // The placement captured before entering: the normal rectangle plus the maximized flag, which a
    // bare SetWindowPos cannot reproduce.
    WINDOWPLACEMENT placement{};
    DWORD style = 0;
    bool placement_valid = false;
};

// The window's outer rectangle (position and size) in physical pixels, which is what
// `SetWindowPos`/`GetWindowRect` speak. The caller converts it to logical pixels for the metrics.
[[nodiscard]] WindowRect win32_window_bounds(HWND window) noexcept;

// True when `window`'s outer rectangle covers its monitor; the answer the metrics report, so a
// state the user changed (F11 handled by a shell, a window manager shortcut) is observed, not
// assumed.
[[nodiscard]] bool win32_is_fullscreen(HWND window) noexcept;

// Remembers the placement only while the window is not already full-screen, so a repeated enter
// cannot capture the full-screen rectangle as the restore target.
void win32_fullscreen_enter(HWND window, Win32FullscreenState& state) noexcept;

// Leaves full-screen and puts the window back at the saved placement (position, size and the
// restored/maximized flag). A window whose saved record is unusable keeps the platform's default
// restored placement.
void win32_fullscreen_exit(HWND window, Win32FullscreenState& state) noexcept;

// The bounds `win32_fullscreen_exit` would restore, if it has a usable record. Read before exiting,
// because the exit consumes it.
[[nodiscard]] bool win32_fullscreen_has_restore(HWND window,
                                                const Win32FullscreenState& state) noexcept;

// Enter or leave, and report whether the resolved mode actually changed (so the caller enqueues one
// Resize). A repeated request is a no-op that reports false, and a window on the wrong thread is
// ignored for the same reason as every other mutating call on this seam.
[[nodiscard]] bool win32_apply_fullscreen(HWND window, Win32FullscreenState& state,
                                          bool fullscreen) noexcept;

} // namespace tessera::hal::detail

namespace tessera::hal::detail {

// The full-screen entry point window.cpp forwards to: owner-thread guarded, no-op on a null window,
// and true when the resolved mode changed (so the caller enqueues one Resize; WM_SIZE fires too,
// but both describe the same change and coalescing them keeps the engine on its one repaint path).
[[nodiscard]] bool win32_window_set_fullscreen(HWND window, Win32FullscreenState& state,
                                               bool fullscreen, DWORD owner_thread) noexcept;

} // namespace tessera::hal::detail
