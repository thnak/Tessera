// SPDX-License-Identifier: MIT
//
// Win32 DPI arithmetic and create-time client sizing (docs/specs/os-events-v1.md section 4; ADR-023
// rule 4). Split from `window_display_win.cpp` (the per-monitor reads) so both stay inside the
// file-size budget; `window.cpp` is well past its own limit, so nothing display- or DPI-related
// belongs there.
//
// These are the conversions every logical/physical decision goes through: the metrics build, the
// hit test's logical coordinates, and the initial window size. They are pure arithmetic over a DPI
// value, so a caller can reason about (and test) them without a monitor.

// The Win32 headers define `min`/`max` as macros unless NOMINMAX is set before them; the guard has
// to come first (window.cpp does the same).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "window_display_win.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::hal::detail {

UINT win32_window_dpi(HWND window) noexcept {
    const UINT dpi = window == nullptr ? 0 : GetDpiForWindow(window);
    return dpi == 0 ? kDefaultWindowDpi : dpi;
}

float win32_dpi_scale(UINT dpi) noexcept {
    const UINT usable = dpi == 0 ? kDefaultWindowDpi : dpi;
    return static_cast<float>(usable) / static_cast<float>(kDefaultWindowDpi);
}

std::uint32_t win32_logical_from_physical(std::uint32_t physical, UINT dpi) noexcept {
    const UINT usable = dpi == 0 ? kDefaultWindowDpi : dpi;
    const double scaled = static_cast<double>(physical) * static_cast<double>(kDefaultWindowDpi) /
                          static_cast<double>(usable);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

std::uint32_t win32_physical_from_logical(std::uint32_t logical, UINT dpi) noexcept {
    const UINT usable = dpi == 0 ? kDefaultWindowDpi : dpi;
    const double scaled = static_cast<double>(logical) * static_cast<double>(usable) /
                          static_cast<double>(kDefaultWindowDpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

RECT win32_outer_rect_for_logical(std::uint32_t logical_width, std::uint32_t logical_height,
                                  UINT dpi, DWORD style) noexcept {
    const UINT usable = dpi == 0 ? kDefaultWindowDpi : dpi;
    RECT rect{0, 0, static_cast<LONG>(win32_physical_from_logical(logical_width, usable)),
              static_cast<LONG>(win32_physical_from_logical(logical_height, usable))};
    if (AdjustWindowRectExForDpi(&rect, style, FALSE, 0, usable) == FALSE) {
        // A failed adjustment still yields a usable client size, which is what CreateWindowExW
        // accepted before this helper existed.
        rect = RECT{0, 0, static_cast<LONG>(win32_physical_from_logical(logical_width, usable)),
                    static_cast<LONG>(win32_physical_from_logical(logical_height, usable))};
    }
    return rect;
}

void win32_normalize_client_size(HWND window, std::uint32_t logical_width,
                                 std::uint32_t logical_height, UINT create_dpi,
                                 DWORD style) noexcept {
    if (window == nullptr) {
        return;
    }
    const UINT actual_dpi = win32_window_dpi(window);
    if (actual_dpi == create_dpi) {
        return;
    }
    const RECT target =
        win32_outer_rect_for_logical(logical_width, logical_height, actual_dpi, style);
    SetWindowPos(window, nullptr, 0, 0, static_cast<int>(target.right - target.left),
                 static_cast<int>(target.bottom - target.top),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace tessera::hal::detail
