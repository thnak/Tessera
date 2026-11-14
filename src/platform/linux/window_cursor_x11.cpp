// SPDX-License-Identifier: MIT
//
// X11 pointer shapes for the HAL window (issue #191).
//
// The X server owns the cursors: a client asks for one from the `cursorfont` glyph set
// (`XCreateFontCursor`, which is a server-side resource created from the standard `cursor` font)
// and installs it on a window with `XDefineCursor`. Both are buffered requests, so nothing here
// waits for a round trip or calls `XSync`; the pointer changes on the server's next pass.
//
// The cache is per window and per shape: the first request for a shape creates the cursor, and
// every later request for it is one `XDefineCursor`. `Hidden` (CSS `cursor: none`) is the one shape
// with no cursor at all: `XDefineCursor(display, window, None)` removes the pointer for that
// window.

#include "window_cursor_x11.hpp"

#include <X11/cursorfont.h>
#include <cstddef>

namespace tessera::hal::detail {
namespace {

// The `cursorfont` glyph for a shape. Shapes X11 has no glyph for map to the nearest conventional
// equivalent (the hand for interactive intent, the double arrows for a two-sided resize), which is
// the same "nearest equivalent" policy the Win32 table uses.
unsigned int glyph_for(CursorShape shape) noexcept {
    switch (shape) {
    case CursorShape::Hidden:
        // No glyph: handled by the caller as None.
        return 0;
    case CursorShape::Default:
        return XC_left_ptr;
    case CursorShape::ContextMenu:
        return XC_question_arrow;
    case CursorShape::Help:
        return XC_question_arrow;
    case CursorShape::Pointer:
        return XC_hand2;
    case CursorShape::Progress:
        return XC_watch;
    case CursorShape::Wait:
        return XC_watch;
    case CursorShape::Cell:
        return XC_crosshair;
    case CursorShape::Crosshair:
        return XC_crosshair;
    case CursorShape::Text:
        return XC_xterm;
    case CursorShape::VerticalText:
        return XC_xterm;
    case CursorShape::Alias:
        return XC_hand2;
    case CursorShape::Copy:
        return XC_hand2;
    case CursorShape::Move:
        return XC_fleur;
    case CursorShape::NoDrop:
        return XC_circle;
    case CursorShape::NotAllowed:
        return XC_circle;
    case CursorShape::Grab:
        return XC_hand1;
    case CursorShape::Grabbing:
        return XC_hand2;
    case CursorShape::AllScroll:
        return XC_fleur;
    case CursorShape::ZoomIn:
        return XC_hand2;
    case CursorShape::ZoomOut:
        return XC_hand2;
    case CursorShape::ResizeN:
        return XC_sb_v_double_arrow;
    case CursorShape::ResizeE:
        return XC_sb_h_double_arrow;
    case CursorShape::ResizeS:
        return XC_sb_v_double_arrow;
    case CursorShape::ResizeW:
        return XC_sb_h_double_arrow;
    case CursorShape::ResizeNe:
        return XC_top_right_corner;
    case CursorShape::ResizeNw:
        return XC_top_left_corner;
    case CursorShape::ResizeSe:
        return XC_bottom_right_corner;
    case CursorShape::ResizeSw:
        return XC_bottom_left_corner;
    case CursorShape::ResizeNs:
        return XC_sb_v_double_arrow;
    case CursorShape::ResizeEw:
        return XC_sb_h_double_arrow;
    case CursorShape::ResizeNesw:
        return XC_sb_v_double_arrow;
    case CursorShape::ResizeNwse:
        return XC_sb_h_double_arrow;
    case CursorShape::ColResize:
        return XC_sb_h_double_arrow;
    case CursorShape::RowResize:
        return XC_sb_v_double_arrow;
    }
    // Unreachable with an in-range enumerator; the arrow is the safe visual fallback.
    return XC_left_ptr;
}

} // namespace

Cursor x11_cursor_for(Display* display, X11CursorState& state, CursorShape shape) noexcept {
    if (display == nullptr) {
        return None;
    }
    const std::size_t index = static_cast<std::size_t>(shape);
    if (index >= kCursorShapeCount) {
        return None;
    }
    if (shape == CursorShape::Hidden) {
        // No cursor: `XDefineCursor` with None is how this window's pointer disappears.
        return None;
    }
    if (!state.created[index]) {
        const unsigned int glyph = glyph_for(shape);
        // XCreateFontCursor returns None when the `cursor` font is unavailable; caching that as
        // "created" keeps a stripped server from retrying the allocation on every pointer move. The
        // caller then leaves the pointer alone rather than installing a null cursor by mistake.
        state.cursors[index] = XCreateFontCursor(display, glyph);
        state.created[index] = true;
    }
    return state.cursors[index];
}

void x11_cursor_apply(Display* display, ::Window window, X11CursorState& state,
                      CursorShape shape) noexcept {
    if (display == nullptr || window == 0) {
        return;
    }
    if (shape == CursorShape::Hidden) {
        // None removes the pointer for this window; the server keeps no cursor attached.
        XDefineCursor(display, window, None);
        return;
    }
    const Cursor cursor = x11_cursor_for(display, state, shape);
    if (cursor == None) {
        // The glyph could not be created (no `cursor` font). Leaving the current pointer alone is
        // the documented fallback; installing None here would accidentally hide it.
        return;
    }
    XDefineCursor(display, window, cursor);
}

void x11_cursor_release(Display* display, X11CursorState& state) noexcept {
    if (display != nullptr) {
        for (std::size_t index = 0; index < kCursorShapeCount; ++index) {
            if (state.created[index] && state.cursors[index] != None) {
                XFreeCursor(display, state.cursors[index]);
            }
        }
    }
    // Clear even without a display: the connection is going away either way.
    for (std::size_t index = 0; index < kCursorShapeCount; ++index) {
        state.cursors[index] = None;
        state.created[index] = false;
    }
}

} // namespace tessera::hal::detail
