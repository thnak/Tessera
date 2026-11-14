// SPDX-License-Identifier: MIT

#pragma once

// Win32 pointer shapes for the HAL window (issue #191, docs/specs/window-chrome-v1.md section 8.5).
// Lives in its own translation unit because `window.cpp` is already at the file-size limit; the
// window class holds one shape value and calls these functions.
//
// A TU that includes `<windows.h>` must define the two lean/nominmax macros first, exactly like
// window.cpp and window_fullscreen_win.hpp do.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "tessera/hal/cursor.hpp"

#include <optional>
#include <windows.h>

namespace tessera::hal::detail {

// The stock cursor handle for `shape`: `LoadCursorW` on first use, cached per process (the stock
// cursors are shared resources, so they are never destroyed). Null only for `CursorShape::Hidden`,
// which has no handle: hiding is `SetCursor(nullptr)`, and a null return tells the caller that.
[[nodiscard]] HCURSOR win32_cursor_for(CursorShape shape) noexcept;

// Installs `shape` on `window` right away. This is the path a programmatic change takes
// (`set_cursor`), and it is what makes the shape visible while the pointer is already inside the
// client area: the next `WM_SETCURSOR` would otherwise arrive only after the pointer leaves and
// re-enters. A null window or a failed `SetCursor` is a no-op, never an error.
void win32_apply_cursor(HWND window, CursorShape shape) noexcept;

// Answers one `WM_SETCURSOR` for the window whose current request is `shape`. Returns the result to
// hand back to Windows when the message is this window's to answer, and `std::nullopt` when
// DefWindowProcW keeps it (the non-client area, another window's message), so the system frame
// keeps its own resize arrows. `WM_SETCURSOR` is where Windows re-asserts the cursor, including
// after any non-client transition, so this is the one place that must install the shape again.
[[nodiscard]] std::optional<LRESULT>
win32_cursor_message(UINT message, WPARAM wparam, LPARAM lparam, CursorShape shape) noexcept;

} // namespace tessera::hal::detail
