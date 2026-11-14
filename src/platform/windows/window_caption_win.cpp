// SPDX-License-Identifier: MIT
//
// Win32 non-client hit testing for the opt-in custom title bar (window-chrome-v1.md section 8.1).
//
// Two rules make this a HAL concern rather than a scene one:
//
//   * The window must stay resizable. A client-side caption removes the OS frame, so the resize
//     border has to be synthesized here, and it wins over a caption region so a control parked in
//     the corner never takes away the resize handle.
//   * The answer is an HT* code, never a scene node. The framework says *where* its controls are
//     (logical pixels, region kinds); this file says *what* Windows does with a hit.
//
// Coordinates: WM_NCHITTEST reports screen pixels, the regions are logical client pixels, so every
// comparison converts through the window's DPI and client origin.

#include "window_caption_win.hpp"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace tessera::hal::detail {
namespace {

float dpi_scale_of(int dpi) noexcept {
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

// Screen point to logical client pixels: GetClientRect's origin is the client area, and
// ScreenToClient gives its position on screen.
bool to_logical_client(HWND window, POINT screen_point, float scale, float& x, float& y) noexcept {
    POINT client = screen_point;
    if (window == nullptr || ScreenToClient(window, &client) == FALSE) {
        return false;
    }
    x = static_cast<float>(client.x) / scale;
    y = static_cast<float>(client.y) / scale;
    return true;
}

int resize_border_physical(int dpi) noexcept {
    const int scaled = static_cast<int>(std::lround(kCaptionResizeBorder * dpi_scale_of(dpi)));
    return std::max(scaled, 1);
}

// The resize-border answer for a point in physical client pixels, or 0 when the point is not on the
// border. A maximized window has no border: Windows disables resizing there, and keeping the edge
// for a region is what lets a control sit flush against it.
LRESULT border_hit(HWND window, POINT local, LONG width, LONG height, int dpi) noexcept {
    if (IsZoomed(window) != FALSE) {
        return 0;
    }
    const int border = resize_border_physical(dpi);
    if (local.x < border) {
        return local.y < border ? HTTOPLEFT : (local.y >= height - border ? HTBOTTOMLEFT : HTLEFT);
    }
    if (local.x >= width - border) {
        return local.y < border ? HTTOPRIGHT
                                : (local.y >= height - border ? HTBOTTOMRIGHT : HTRIGHT);
    }
    if (local.y < border) {
        return HTTOP;
    }
    if (local.y >= height - border) {
        return HTBOTTOM;
    }
    return 0;
}

} // namespace

void win32_set_caption_regions(HWND window, Win32CaptionState& state,
                               std::span<const CaptionRegion> regions) noexcept {
    state.regions = make_caption_regions(regions, state.count);
    // A custom caption needs the non-client area to be client area; the framework's regions then
    // provide drag and control answers. The hit test is resolved per message, so publishing regions
    // is free until the next hit; the frame change is what makes WM_NCCALCSIZE report no frame.
    if (window != nullptr && state.custom()) {
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

bool win32_custom_caption_nc_calc_size(const Win32CaptionState& state, WPARAM wparam) noexcept {
    // wparam == FALSE spells "no client-area rectangle to compute" (a frame-only change), which
    // keeps the documented default handling.
    return state.custom() && wparam != FALSE;
}

bool win32_custom_caption_message(HWND window, const Win32CaptionState& state, UINT message,
                                  WPARAM wparam, LPARAM lparam, int dpi, LRESULT& answer) noexcept {
    if (window == nullptr || !state.custom()) {
        return false;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        if (!win32_custom_caption_nc_calc_size(state, wparam)) {
            return false;
        }
        answer = 0;
        return true;
    case WM_NCHITTEST:
        answer = win32_custom_caption_hit_test(window, state, dpi);
        return true;
    case WM_NCLBUTTONDOWN:
        if (!win32_custom_caption_button_down(window, state, dpi, lparam)) {
            return false;
        }
        answer = 0;
        return true;
    default:
        return false;
    }
}

LRESULT win32_custom_caption_hit_test(HWND window, const Win32CaptionState& state,
                                      int dpi) noexcept {
    if (window == nullptr || !state.custom()) {
        return HTCLIENT;
    }
    RECT client{};
    if (GetClientRect(window, &client) == FALSE) {
        return HTCLIENT;
    }
    POINT screen{};
    if (GetCursorPos(&screen) == FALSE) {
        return HTCLIENT;
    }
    POINT local = screen;
    if (ScreenToClient(window, &local) == FALSE) {
        return HTCLIENT;
    }
    if (const LRESULT border =
            border_hit(window, local, client.right - client.left, client.bottom - client.top, dpi);
        border != 0) {
        return border;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (!to_logical_client(window, screen, dpi_scale_of(dpi), x, y)) {
        return HTCLIENT;
    }
    const CaptionRegion* region = caption_region_at(state.view(), x, y);
    if (region == nullptr) {
        return HTCLIENT;
    }
    switch (region->kind) {
    case CaptionRegionKind::Drag:
        return HTCAPTION;
    case CaptionRegionKind::SystemMenu:
        return HTSYSMENU;
    case CaptionRegionKind::Minimize:
        return HTMINBUTTON;
    case CaptionRegionKind::Maximize:
        // The distinct code is what makes Snap Layouts fly out on Windows 11; Windows 10 treats it
        // as a caption button like any other.
        return HTMAXBUTTON;
    case CaptionRegionKind::Close:
        return HTCLOSE;
    }
    return HTCLIENT;
}

bool win32_custom_caption_button_down(HWND window, const Win32CaptionState& state, int dpi,
                                      LPARAM lparam) noexcept {
    if (window == nullptr || !state.custom()) {
        return false;
    }
    const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    float x = 0.0f;
    float y = 0.0f;
    if (!to_logical_client(window, screen, dpi_scale_of(dpi), x, y)) {
        return false;
    }
    const CaptionRegion* region = caption_region_at(state.view(), x, y);
    if (region == nullptr || !is_caption_button(region->kind)) {
        // Drag and system-menu regions keep the OS behaviour (the modal move loop, the window
        // menu).
        return false;
    }
    win32_perform_caption_button(window, region->kind);
    return true;
}

bool win32_caption_button_at(HWND window, const Win32CaptionState& state, int dpi,
                             POINT screen_point) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    if (window == nullptr || !state.custom() ||
        !to_logical_client(window, screen_point, dpi_scale_of(dpi), x, y)) {
        return false;
    }
    const CaptionRegion* region = caption_region_at(state.view(), x, y);
    return region != nullptr && is_caption_button(region->kind);
}

void win32_perform_caption_button(HWND window, CaptionRegionKind kind) noexcept {
    if (window == nullptr) {
        return;
    }
    switch (kind) {
    case CaptionRegionKind::Minimize:
        ShowWindow(window, SW_MINIMIZE);
        return;
    case CaptionRegionKind::Maximize:
        ShowWindow(window, IsZoomed(window) != FALSE ? SW_RESTORE : SW_MAXIMIZE);
        return;
    case CaptionRegionKind::Close:
        // The same WM_CLOSE the OS posts for its own close button, so graceful shutdown stays one
        // path (window.cpp turns it into the normalized Close event).
        PostMessageW(window, WM_CLOSE, 0, 0);
        return;
    case CaptionRegionKind::Drag:
    case CaptionRegionKind::SystemMenu:
        return;
    }
}

} // namespace tessera::hal::detail
