// SPDX-License-Identifier: MIT
//
// Win32 full-screen for the HAL window (issue #232).
//
// Entering uses the documented borderless path: remember the placement, drop the overlapped style
// to a popup with no frame, and cover the monitor's full rectangle. Exiting reapplies the saved
// placement, so the window returns to exactly the position, size and maximized/restored state it
// had, never to a guess. `SWP_FRAMECHANGED` is what makes the style change take effect without a
// resize message storm.
//
// The monitor is the one the window is on, not the primary one: on a multi-monitor desktop a
// full-screen window must cover the display the user put it on (`MonitorFromWindow` with
// `MONITOR_DEFAULTTONEAREST` matches the display-refresh query in window.cpp).

#include "window_fullscreen_win.hpp"

#include <algorithm>

namespace tessera::hal::detail {
namespace {

// A window is full-screen when its outer rectangle covers its monitor. Comparing the monitor
// rectangle rather than trusting a flag is what makes an F11 or a shell shortcut visible: the
// window's own idea of the mode is never consulted.
bool covers_monitor(HWND window) noexcept {
    if (window == nullptr || IsIconic(window) != FALSE || IsZoomed(window) != FALSE) {
        // A maximized window is one pixel-border away from the monitor and is not full-screen.
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return false;
    }
    RECT outer{};
    if (GetWindowRect(window, &outer) == FALSE) {
        return false;
    }
    return outer.left == info.rcMonitor.left && outer.top == info.rcMonitor.top &&
           outer.right == info.rcMonitor.right && outer.bottom == info.rcMonitor.bottom;
}

} // namespace

WindowRect win32_window_bounds(HWND window) noexcept {
    RECT rect{};
    if (window == nullptr || GetWindowRect(window, &rect) == FALSE) {
        return {};
    }
    return WindowRect{static_cast<float>(rect.left), static_cast<float>(rect.top),
                      static_cast<float>(std::max<LONG>(rect.right - rect.left, 0)),
                      static_cast<float>(std::max<LONG>(rect.bottom - rect.top, 0))};
}

bool win32_is_fullscreen(HWND window) noexcept {
    return covers_monitor(window);
}

void win32_fullscreen_enter(HWND window, Win32FullscreenState& fullscreen) noexcept {
    if (window == nullptr) {
        return;
    }
    // A repeated enter keeps the first capture: the live rectangle is already full-screen.
    if (!fullscreen_enter(fullscreen.state, win32_window_bounds(window))) {
        return;
    }
    fullscreen.placement = WINDOWPLACEMENT{};
    fullscreen.placement.length = sizeof(WINDOWPLACEMENT);
    fullscreen.placement_valid = GetWindowPlacement(window, &fullscreen.placement) != FALSE;
    if (!fullscreen.placement_valid) {
        fullscreen.placement = WINDOWPLACEMENT{};
        fullscreen.placement.length = sizeof(WINDOWPLACEMENT);
    }
    fullscreen.style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        // The window is already borderless; without the monitor rectangle, cover the desktop.
        info.rcMonitor = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    SetWindowLongPtrW(window, GWL_STYLE,
                      static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN));
    SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void win32_fullscreen_exit(HWND window, Win32FullscreenState& fullscreen) noexcept {
    if (window == nullptr) {
        return;
    }
    const bool had_bounds = fullscreen_has_restore_bounds(fullscreen.state);
    if (!fullscreen_exit(fullscreen.state)) {
        return;
    }
    const WINDOWPLACEMENT saved = fullscreen.placement;
    const DWORD saved_style = fullscreen.style;
    const bool placement_valid = fullscreen.placement_valid;
    fullscreen.placement = WINDOWPLACEMENT{};
    fullscreen.placement.length = sizeof(WINDOWPLACEMENT);
    fullscreen.placement_valid = false;

    // Restore the overlapped style before the placement, so SetWindowPlacement's normal rectangle
    // is interpreted with the frame the window had before entering.
    SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(saved_style));
    WINDOWPLACEMENT placement = saved;
    placement.length = sizeof(WINDOWPLACEMENT);
    // showCmd is left as captured: forcing SW_SHOWNORMAL would break "was maximized before".
    if (had_bounds && placement_valid &&
        placement.rcNormalPosition.right > placement.rcNormalPosition.left &&
        placement.rcNormalPosition.bottom > placement.rcNormalPosition.top &&
        SetWindowPlacement(window, &placement) != FALSE) {
        return;
    }
    // No usable record: the window keeps its current size, now with its normal frame, and the
    // caller's metrics report the resolved state. Showing it is the platform's job, not the app's.
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    ShowWindow(window, SW_SHOWNORMAL);
}

bool win32_fullscreen_has_restore(HWND window, const Win32FullscreenState& state) noexcept {
    return window != nullptr && fullscreen_has_restore_bounds(state.state) && state.placement_valid;
}

bool win32_apply_fullscreen(HWND window, Win32FullscreenState& state, bool fullscreen) noexcept {
    if (window == nullptr) {
        return false;
    }
    const bool was_active = win32_is_fullscreen(window);
    if (fullscreen) {
        win32_fullscreen_enter(window, state);
    } else {
        win32_fullscreen_exit(window, state);
    }
    return was_active != win32_is_fullscreen(window);
}

bool win32_window_set_fullscreen(HWND window, Win32FullscreenState& state, bool fullscreen,
                                 DWORD owner_thread) noexcept {
    if (window == nullptr || GetCurrentThreadId() != owner_thread) {
        // The owner-thread rule is the same as every other mutating call on this seam: a foreign
        // thread would race the message loop with a style change.
        return false;
    }
    return win32_apply_fullscreen(window, state, fullscreen);
}

} // namespace tessera::hal::detail
