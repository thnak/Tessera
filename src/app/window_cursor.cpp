// SPDX-License-Identifier: MIT
//
// The app half of the CSS cursor seam (issue #191): map the scene's resolved `CursorKind` onto the
// HAL's `CursorShape` and request it from the platform window, only when it changed.
//
// Why the mapping lives here and not in `ui/`: a UI-layer header must not include a `hal::` one
// (ADR-020), and the HAL must not know a CSS keyword. This is the one place both enums exist, which
// is exactly what the three-layer chain wants.

#include "window_cursor.hpp"

#include <cstdio>

namespace tessera::app::detail {
namespace {

// The diagnostic a platform without a pointer-shape source owes its caller. One line, matching the
// TESSERA_WINDOW_UNSUPPORTED / TESSERA_GLASS_WINDOW_UNSUPPORTED family, with the shape the caller
// wanted so the log names what was ignored.
void report_unsupported_cursor(hal::CursorShape wanted) {
    std::fprintf(
        stderr,
        "TESSERA_WINDOW_CURSOR_UNSUPPORTED: the CSS cursor property is unavailable on this "
        "platform (shape %.*s ignored)\n",
        static_cast<int>(hal::cursor_shape_name(wanted).size()),
        hal::cursor_shape_name(wanted).data());
}

} // namespace

hal::CursorShape to_hal_cursor(ui::style::CursorKind kind) noexcept {
    using ui::style::CursorKind;
    switch (kind) {
    case CursorKind::Auto:
    case CursorKind::Default:
        return hal::CursorShape::Default;
    case CursorKind::None:
        return hal::CursorShape::Hidden;
    case CursorKind::ContextMenu:
        return hal::CursorShape::ContextMenu;
    case CursorKind::Help:
        return hal::CursorShape::Help;
    case CursorKind::Pointer:
        return hal::CursorShape::Pointer;
    case CursorKind::Progress:
        return hal::CursorShape::Progress;
    case CursorKind::Wait:
        return hal::CursorShape::Wait;
    case CursorKind::Cell:
        return hal::CursorShape::Cell;
    case CursorKind::Crosshair:
        return hal::CursorShape::Crosshair;
    case CursorKind::Text:
        return hal::CursorShape::Text;
    case CursorKind::VerticalText:
        return hal::CursorShape::VerticalText;
    case CursorKind::Alias:
        return hal::CursorShape::Alias;
    case CursorKind::Copy:
        return hal::CursorShape::Copy;
    case CursorKind::Move:
        return hal::CursorShape::Move;
    case CursorKind::NoDrop:
        return hal::CursorShape::NoDrop;
    case CursorKind::NotAllowed:
        return hal::CursorShape::NotAllowed;
    case CursorKind::Grab:
        return hal::CursorShape::Grab;
    case CursorKind::Grabbing:
        return hal::CursorShape::Grabbing;
    case CursorKind::AllScroll:
        return hal::CursorShape::AllScroll;
    case CursorKind::ZoomIn:
        return hal::CursorShape::ZoomIn;
    case CursorKind::ZoomOut:
        return hal::CursorShape::ZoomOut;
    case CursorKind::ResizeN:
        return hal::CursorShape::ResizeN;
    case CursorKind::ResizeE:
        return hal::CursorShape::ResizeE;
    case CursorKind::ResizeS:
        return hal::CursorShape::ResizeS;
    case CursorKind::ResizeW:
        return hal::CursorShape::ResizeW;
    case CursorKind::ResizeNe:
        return hal::CursorShape::ResizeNe;
    case CursorKind::ResizeNw:
        return hal::CursorShape::ResizeNw;
    case CursorKind::ResizeSe:
        return hal::CursorShape::ResizeSe;
    case CursorKind::ResizeSw:
        return hal::CursorShape::ResizeSw;
    case CursorKind::ResizeNs:
        return hal::CursorShape::ResizeNs;
    case CursorKind::ResizeEw:
        return hal::CursorShape::ResizeEw;
    case CursorKind::ResizeNesw:
        return hal::CursorShape::ResizeNesw;
    case CursorKind::ResizeNwse:
        return hal::CursorShape::ResizeNwse;
    case CursorKind::ColResize:
        return hal::CursorShape::ColResize;
    case CursorKind::RowResize:
        return hal::CursorShape::RowResize;
    }
    // Unreachable for an in-range enumerator (the switch is exhaustive). The arrow is the
    // documented fallback for a value cast from an out-of-range integer.
    return hal::CursorShape::Default;
}

hal::CursorShape hovered_cursor(const ui::Node* hovered) noexcept {
    return hovered != nullptr ? to_hal_cursor(hovered->cursor()) : hal::CursorShape::Default;
}

bool apply_cursor(hal::Window& window, hal::CursorShape wanted, hal::CursorShape& last,
                  bool& unsupported_reported) {
    if (wanted == last) {
        // A static hover makes no platform call, which is what keeps the cursor path off the frame
        // budget entirely.
        return false;
    }
    last = wanted;
    if (!window.supports_cursor()) {
        // Report once and remember the decision: the capability cannot change for a window's life,
        // and a button that stays hovered must not spam the log. The app keeps running normally.
        if (!unsupported_reported) {
            unsupported_reported = true;
            report_unsupported_cursor(wanted);
            return true;
        }
        return false;
    }
    window.set_cursor(wanted);
    return false;
}

} // namespace tessera::app::detail
