// SPDX-License-Identifier: MIT

#pragma once

// Win32 non-client hit testing for the opt-in custom title bar (window-chrome-v1.md section 8.1).
// Lives outside window.cpp (file-size limit); the window class owns one state value and forwards
// WM_NCCALCSIZE/WM_NCHITTEST/WM_NCLBUTTONDOWN here.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <windows.h>

#include "tessera/hal/caption.hpp"

namespace tessera::hal::detail {

// The client-area border of a custom-caption window that still resizes: the HAL resolves these to the
// matching HT* resize code. Logical pixels at 100% scale, DPI-scaled at use.
inline constexpr int kCaptionResizeBorder = 4;

// One window's published caption regions. Owned by the window (not a process table) so several
// windows can have different captions (issue #228), and copied so the caller may pass a stack buffer.
struct Win32CaptionState {
    std::array<CaptionRegion, kMaxCaptionRegions> regions{};
    std::uint8_t count = 0;

    [[nodiscard]] std::span<const CaptionRegion> view() const noexcept {
        return std::span<const CaptionRegion>(regions.data(), count);
    }
    [[nodiscard]] bool custom() const noexcept {
        return count > 0;
    }
};

// Publishes the framework's regions. Empty returns the window to the system-frame answer; publishing
// a custom caption applies SWP_FRAMECHANGED so WM_NCCALCSIZE reports no frame. Safe on a null window
// (state only), which is what the tests and a not-yet-created window see.
void win32_set_caption_regions(HWND window, Win32CaptionState& state,
                               std::span<const CaptionRegion> regions) noexcept;

// Answers WM_NCCALCSIZE for a custom-caption window: the engine draws the bar, so the whole window is
// client area. Returns false when the message must go to DefWindowProcW (system frame, wparam FALSE).
[[nodiscard]] bool win32_custom_caption_nc_calc_size(const Win32CaptionState& state,
                                                    WPARAM wparam) noexcept;

// Answers the three non-client messages of a custom caption in one call, so window.cpp's message
// switch stays a single forwarding case: WM_NCCALCSIZE (always answered when it returns true),
// WM_NCHITTEST and WM_NCLBUTTONDOWN (answered only for a caption button). Returns false for every
// other message and for the cases DefWindowProcW still owns (system frame, caption drag, the window
// menu), and stores the answer in `answer` when it returns true.
[[nodiscard]] bool win32_custom_caption_message(HWND window, const Win32CaptionState& state,
                                               UINT message, WPARAM wparam, LPARAM lparam, int dpi,
                                               LRESULT& answer) noexcept;

// Answers one WM_NCHITTEST for a custom-caption window. `dpi` is the window's DPI (96 = 100%).
// Returns the HT* code: the resize border first (a border wins over a region so a window stays
// resizable), then the region under the point, then HTCLIENT.
[[nodiscard]] LRESULT win32_custom_caption_hit_test(HWND window, const Win32CaptionState& state,
                                                   int dpi) noexcept;

// Handles one WM_NCLBUTTONDOWN. Returns true when a caption button owned the press and its window
// action was performed (the caller returns 0); false leaves the message to DefWindowProcW, which
// owns caption drag, double-click maximize, snap and the window menu.
[[nodiscard]] bool win32_custom_caption_button_down(HWND window, const Win32CaptionState& state,
                                                   int dpi, LPARAM lparam) noexcept;

// True when the point (screen coordinates, as WM_NCHITTEST reports) is inside one of the window's
// caption buttons. WM_NCLBUTTONUP uses it to tell a completed button click from a drag that started
// on a button and ended elsewhere.
[[nodiscard]] bool win32_caption_button_at(HWND window, const Win32CaptionState& state, int dpi,
                                          POINT screen_point) noexcept;

// Performs the window action a caption button selects: minimize, maximize/restore, or close. A drag
// or system-menu region is not a button and does nothing here. The OS supplies the maximize/restore
// and close semantics (snap, the window menu, WM_CLOSE), so the HAL never invents window policy.
void win32_perform_caption_button(HWND window, CaptionRegionKind kind) noexcept;

} // namespace tessera::hal::detail
