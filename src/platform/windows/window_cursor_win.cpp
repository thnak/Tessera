// SPDX-License-Identifier: MIT
//
// Win32 pointer shapes for the HAL window (issue #191).
//
// The shapes are the documented stock system cursors: `LoadCursorW(nullptr, IDC_*)` returns a
// shared resource owned by the process-wide cursor table, so nothing here is destroyed and no
// message-time allocation happens. The table is loaded lazily on first use and then cached, which
// keeps the `WM_SETCURSOR` path free of per-message `LoadCursorW` calls.
//
// `WM_SETCURSOR` is the message Windows uses to re-assert the cursor, and it fires again after
// every non-client transition (a border hover, a menu, moving between monitors). Handling it -
// rather than only calling `SetCursor` from the state change - is what keeps the app's shape
// instead of the class cursor snapping back. The answer is only ours when the hit test says the
// pointer is over the client area (`LOWORD(lparam) == HTCLIENT`): over the frame Windows must keep
// its resize arrows, so the message goes back to `DefWindowProcW`.

#include "window_cursor_win.hpp"

#include <array>
#include <cstddef>

namespace tessera::hal::detail {
namespace {

// The stock cursor ids from winuser.h, spelled out through MAKEINTRESOURCEW. The IDC_* macros pick
// MAKEINTRESOURCE or MAKEINTRESOURCEW from UNICODE, and this target does not define UNICODE, so the
// macro would expand to an LPSTR the wide LoadCursorW cannot take (window.cpp spells its own arrow
// the same way). The numbers are stable across Windows versions.
constexpr int kIdcArrow = 32512;
constexpr int kIdcIbeam = 32513;
constexpr int kIdcWait = 32514;
constexpr int kIdcCross = 32515;
constexpr int kIdcSizeNwse = 32642;
constexpr int kIdcSizeNesw = 32643;
constexpr int kIdcSizeWe = 32644;
constexpr int kIdcSizeNs = 32645;
constexpr int kIdcSizeAll = 32646;
constexpr int kIdcNo = 32648;
constexpr int kIdcHand = 32649;
constexpr int kIdcAppStarting = 32650;
constexpr int kIdcHelp = 32651;

HCURSOR load_cursor(int resource) noexcept {
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(resource));
}

// `LoadCursorW(nullptr, IDC_*)` per shape, loaded once. A shape that some Windows version cannot
// load falls back to the arrow, which is the documented "nearest equivalent" answer: the caller
// still gets a visible pointer, and the metrics report Default.
struct StockCursors {
    std::array<HCURSOR, kCursorShapeCount> handles{};
};

StockCursors& stock_cursors() {
    // Function-local static: the load is thread-safe (C++11 magic statics) and happens on the first
    // `WM_SETCURSOR` or `set_cursor`, never on a hot path.
    static StockCursors table = [] {
        StockCursors cursors;
        const HCURSOR arrow = load_cursor(kIdcArrow);
        const HCURSOR hand = load_cursor(kIdcHand);
        const HCURSOR help = load_cursor(kIdcHelp);
        cursors.handles[static_cast<std::size_t>(CursorShape::Default)] = arrow;
        // `Hidden` has no handle: SetCursor(nullptr) hides the pointer. The entry stays null on
        // purpose so `win32_cursor_for` can answer "nothing to install".
        cursors.handles[static_cast<std::size_t>(CursorShape::Hidden)] = nullptr;
        cursors.handles[static_cast<std::size_t>(CursorShape::ContextMenu)] = arrow;
        cursors.handles[static_cast<std::size_t>(CursorShape::Help)] = help;
        cursors.handles[static_cast<std::size_t>(CursorShape::Pointer)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::Progress)] =
            load_cursor(kIdcAppStarting);
        cursors.handles[static_cast<std::size_t>(CursorShape::Wait)] = load_cursor(kIdcWait);
        cursors.handles[static_cast<std::size_t>(CursorShape::Cell)] = load_cursor(kIdcCross);
        cursors.handles[static_cast<std::size_t>(CursorShape::Crosshair)] = load_cursor(kIdcCross);
        cursors.handles[static_cast<std::size_t>(CursorShape::Text)] = load_cursor(kIdcIbeam);
        cursors.handles[static_cast<std::size_t>(CursorShape::VerticalText)] =
            load_cursor(kIdcIbeam);
        // Windows has no alias/copy/zoom stock cursor; the hand is the conventional nearest intent.
        cursors.handles[static_cast<std::size_t>(CursorShape::Alias)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::Copy)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::Move)] = load_cursor(kIdcSizeAll);
        cursors.handles[static_cast<std::size_t>(CursorShape::NoDrop)] = load_cursor(kIdcNo);
        cursors.handles[static_cast<std::size_t>(CursorShape::NotAllowed)] = load_cursor(kIdcNo);
        cursors.handles[static_cast<std::size_t>(CursorShape::Grab)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::Grabbing)] = load_cursor(kIdcSizeAll);
        cursors.handles[static_cast<std::size_t>(CursorShape::AllScroll)] =
            load_cursor(kIdcSizeAll);
        // No magnifier stock cursor: the hand is the closest "this is interactive" answer.
        cursors.handles[static_cast<std::size_t>(CursorShape::ZoomIn)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::ZoomOut)] = hand;
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeN)] = load_cursor(kIdcSizeNs);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeE)] = load_cursor(kIdcSizeWe);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeS)] = load_cursor(kIdcSizeNs);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeW)] = load_cursor(kIdcSizeWe);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeNe)] =
            load_cursor(kIdcSizeNesw);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeNw)] =
            load_cursor(kIdcSizeNwse);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeSe)] =
            load_cursor(kIdcSizeNwse);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeSw)] =
            load_cursor(kIdcSizeNesw);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeNs)] = load_cursor(kIdcSizeNs);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeEw)] = load_cursor(kIdcSizeWe);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeNesw)] =
            load_cursor(kIdcSizeNesw);
        cursors.handles[static_cast<std::size_t>(CursorShape::ResizeNwse)] =
            load_cursor(kIdcSizeNwse);
        cursors.handles[static_cast<std::size_t>(CursorShape::ColResize)] = load_cursor(kIdcSizeWe);
        cursors.handles[static_cast<std::size_t>(CursorShape::RowResize)] = load_cursor(kIdcSizeNs);
        return cursors;
    }();
    return table;
}

} // namespace

HCURSOR win32_cursor_for(CursorShape shape) noexcept {
    const std::size_t index = static_cast<std::size_t>(shape);
    if (index >= kCursorShapeCount) {
        return nullptr;
    }
    const HCURSOR handle = stock_cursors().handles[index];
    if (handle != nullptr) {
        return handle;
    }
    // `Hidden` (null by design) or a stock cursor this system failed to load: no handle to install.
    return nullptr;
}

void win32_apply_cursor(HWND window, CursorShape shape) noexcept {
    if (window == nullptr) {
        return;
    }
    // A null handle means Hidden on a platform that has no hidden shape, or an unloadable stock
    // resource: leave the arrow in place rather than blanking the pointer.
    const HCURSOR handle = win32_cursor_for(shape);
    if (handle != nullptr || shape == CursorShape::Hidden) {
        // `SetCursor(nullptr)` is the documented way to hide the pointer until the next mouse move.
        SetCursor(handle);
    }
}

std::optional<LRESULT> win32_cursor_message(UINT message, WPARAM wparam, LPARAM lparam,
                                            CursorShape shape) noexcept {
    (void)wparam;
    if (message != WM_SETCURSOR) {
        return std::nullopt;
    }
    // LOWORD(lparam) is the hit-test code: HTCLIENT is the client area this window owns, and only
    // there may the app's shape replace the system one. Everything else (the frame, the caption,
    // the scrollbar) keeps DefWindowProcW's cursor, which is the resize arrows a custom-caption
    // window needs for its edges.
    if (LOWORD(lparam) != HTCLIENT) {
        return std::nullopt;
    }
    const HCURSOR handle = win32_cursor_for(shape);
    if (handle == nullptr && shape != CursorShape::Hidden) {
        // No stock cursor loaded for this shape: let Windows draw its own client cursor rather than
        // answering "handled" with nothing installed.
        return std::nullopt;
    }
    // Returning TRUE tells Windows the cursor is set; a false return would make it install the
    // class cursor over ours.
    SetCursor(handle);
    return TRUE;
}

} // namespace tessera::hal::detail
