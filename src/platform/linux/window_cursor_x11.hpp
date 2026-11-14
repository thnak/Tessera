// SPDX-License-Identifier: MIT

#pragma once

// X11 pointer shapes for the HAL window (issue #191, docs/specs/window-chrome-v1.md section 8.5).
// Lives in its own translation unit because src/platform/linux/window.cpp is at the file-size
// limit.
//
// Xlib's `Window` (an XID) and `tessera::hal::Window` (the platform seam class) share a name; every
// XID here is spelled `::Window` so there is no ambiguity in either direction (the same rule
// window_fullscreen_x11.hpp follows).

#include "tessera/hal/cursor.hpp"

#include <X11/Xlib.h>

// Xlib's X.h defines `CursorShape` as an object-like macro (the glyph-cursor shape argument of
// XCreateGlyphCursor). A macro is not namespace-qualified, so it would rewrite every
// `CursorShape` in this header into `0` and break the HAL enum; nothing here calls the macro, so
// it is dropped after the include. Spelling the type `hal::CursorShape` cannot avoid it the way
// `::Window` avoids the XID typedef, which is why this differs from the window-id rule above.
#ifdef CursorShape
#undef CursorShape
#endif

namespace tessera::hal::detail {

// Per-window cursor cache. The font cursors are server-side resources created on first use, so a
// hover that cycles through a handful of shapes allocates nothing after the first pass; `cursors`
// is sized by `kCursorShapeCount`, so a shape can never index past the table.
struct X11CursorState {
    Cursor cursors[kCursorShapeCount]{};
    // None means "not created yet", not "hidden": `Hidden` draws no cursor, which on X11 is
    // `XDefineCursor(display, window, None)`, so its cache entry stays None and is never created.
    bool created[kCursorShapeCount]{};
};

// Creates the font cursor for `shape` on first use and returns it; None for `Hidden` and for a
// display that cannot create it (never an error). `x11_cursor_apply` is what actually installs it.
[[nodiscard]] Cursor x11_cursor_for(Display* display, X11CursorState& state,
                                    CursorShape shape) noexcept;

// Installs `shape` on `window` with `XDefineCursor`. `Hidden` passes None, which the server
// interprets as "no cursor": the pointer stays invisible while it is over this window. Xlib buffers
// the request, so neither this nor the creation path is a blocking round trip; nothing here calls
// `XSync`. A null display or window is a no-op.
void x11_cursor_apply(Display* display, ::Window window, X11CursorState& state,
                      CursorShape shape) noexcept;

// Frees every cursor the state created and clears it. Called from the window destructor BEFORE
// `XCloseDisplay`, because a cursor is a resource of that connection: freeing after the close would
// leak it server-side. Idempotent.
void x11_cursor_release(Display* display, X11CursorState& state) noexcept;

} // namespace tessera::hal::detail
