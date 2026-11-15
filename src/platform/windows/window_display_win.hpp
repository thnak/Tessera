// SPDX-License-Identifier: MIT
//
// Win32 display-capability reads and display/settings message classification for one window
// (docs/specs/os-events-v1.md section 4; ADR-023 rules 4-7).
//
// This is the platform half only: it reads the OS and reports what it could read, and the caller
// folds the result into the normalized event stream. `window.cpp` is over the file-size limit, so
// everything display-related lives here, split across two translation units:
//   - `window_display_win.cpp`: DPI arithmetic, per-monitor reads, the refresh query, the
//     create-time client-size normalization;
//   - `window_display_messages_win.cpp`: the message -> event classification and the monitor
//     device notification registration.
//
// The rules the raw reads feed are platform-neutral (`tessera/hal/display.hpp`,
// `src/hal/window/display.cpp`), so the fallbacks are tested without a window: this TU never
// decides that 60 Hz means "unknown", it only says whether it could read a mode.

#pragma once

#include "tessera/hal/appearance.hpp"
#include "tessera/hal/display.hpp"
#include "tessera/hal/event_reason.hpp"
#include "tessera/hal/window.hpp"

// The Windows SDK headers must be included in this order: `dbt.h` (the device-broadcast types this
// header uses) does not include `windows.h` itself, so it has to follow it. clang-format's
// SortIncludes would alphabetize `dbt.h` first and break the build, hence `// clang-format off`.
// windows.h is also kept warm for every including TU (window.cpp relies on it transitively).
// clang-format off
#include <windows.h>
#include <dbt.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::hal::detail {

// --- DPI conversion ---------------------------------------------------------------------------
//
// Logical pixels are the engine's space; Win32 reports DPI and physical pixels. These pure helpers
// are shared by the window's metrics build, its hit test and its creation path.

inline constexpr UINT kDefaultWindowDpi = 96;

// The window's DPI through `GetDpiForWindow`, or the system default when Windows cannot answer.
[[nodiscard]] UINT win32_window_dpi(HWND window) noexcept;

// dpi / 96, the device scale every logical/physical conversion uses.
[[nodiscard]] float win32_dpi_scale(UINT dpi) noexcept;

// Physical pixels -> logical pixels at `dpi`, rounded to nearest.
[[nodiscard]] std::uint32_t win32_logical_from_physical(std::uint32_t physical, UINT dpi) noexcept;

// Logical pixels -> physical pixels at `dpi`, rounded to nearest.
[[nodiscard]] std::uint32_t win32_physical_from_logical(std::uint32_t logical, UINT dpi) noexcept;

// The outer window rectangle that holds `logical_width x logical_height` client pixels at `dpi`.
// A failed `AdjustWindowRectExForDpi` falls back to the client size itself.
[[nodiscard]] RECT win32_outer_rect_for_logical(std::uint32_t logical_width,
                                                std::uint32_t logical_height, UINT dpi,
                                                DWORD style) noexcept;

// Resizes an existing window so its client area is the logical size at the monitor it landed on.
// Windows applies the *system* DPI to a window created with `CW_USEDEFAULT`, so the initial monitor
// may need this normalization to keep `WindowDesc` logical pixels. A no-op when the two DPIs agree.
void win32_normalize_client_size(HWND window, std::uint32_t logical_width,
                                 std::uint32_t logical_height, UINT create_dpi,
                                 DWORD style) noexcept;

// Widens UTF-8 to UTF-16 for a Win32 API, falling back to the process code page when the text is
// not valid UTF-8 (titles are display-only, so dropping them would be worse).
[[nodiscard]] std::wstring win32_widen(std::string_view text);

// --- Display capabilities ---------------------------------------------------------------------

// A display message the window must record, and whether DefWindowProc still owns it.
struct DisplayMessage {
    // False when the message is not a display/settings notification this slice handles.
    bool handled = false;
    // True when the message must still reach DefWindowProcW (a settings broadcast whose other
    // parameters are not ours).
    bool forward = false;
    WindowEventType event = WindowEventType::DisplayChanged;
    EventReason reason = EventReason::Unspecified;
};

// Classifies the display/settings notifications into the one event each means
// (docs/specs/os-events-v1.md section 4). Keeping the mapping out of `window.cpp` is what stops
// that file growing every time a display event is added. `lparam` is only read for the two
// `WM_SETTINGCHANGE` areas this slice distinguishes (work area and the immersive color set), and it
// is compared, never dereferenced when the message does not carry an area string.
[[nodiscard]] DisplayMessage classify_win32_display_message(UINT message, WPARAM wparam,
                                                            LPARAM lparam = 0) noexcept;

// Registers the window for monitor device notifications (`WM_DEVICECHANGE`), which is what makes a
// hotplug observable at all on Windows: `WM_DISPLAYCHANGE` alone cannot tell an add from a remove.
// Returns a cookie to pass to `unregister_win32_display_notifications`, or nullptr, in which case
// the window still reports `WM_DISPLAYCHANGE` and simply cannot distinguish a hotplug.
[[nodiscard]] HDEVNOTIFY register_win32_display_notifications(HWND window) noexcept;

// Best-effort unregister; a null cookie is a no-op.
void unregister_win32_display_notifications(HDEVNOTIFY notify) noexcept;

// The display capabilities of the monitor `window` is on, as this build can read them. Every
// field it could not read keeps the documented fallback and sets its `*_assumed` flag, so the
// engine can tell a measured value from a guess (ADR-023 rule 7). `metrics` in/out: the caller's
// snapshot is filled in, not replaced, so the fields the window owns (size, focus, cursor) stay.
void fill_win32_display_capabilities(HWND window, WindowMetrics& metrics) noexcept;

// Fills `out` with the current monitor topology and returns the count written (at most
// `kMaxDisplays`). A monitor name is the GDI device name (`\\.\DISPLAY1`); the primary flag is the
// one `GetMonitorInfoW` sets. Returns 0 (and leaves `out` untouched) when enumeration fails.
std::size_t query_win32_displays(std::span<DisplayInfo> out) noexcept;

// The display id this build assigns to `monitor`. Stable for the monitor's lifetime and equal to
// the id `query_win32_displays` reports for the same monitor: the GDI device name is hashed into a
// 64-bit value, so a `DisplayAdded`/`DisplayRemoved` event names the same display the topology list
// describes. 0 for a null monitor.
[[nodiscard]] DisplayId win32_display_id(HMONITOR monitor) noexcept;

// The display's current refresh, in Hz, or `kUnknownRefreshHz`. This is the creation-time query
// behind `Window::display_refresh_hz()`; the per-event snapshot uses
// `fill_win32_display_capabilities`.
[[nodiscard]] double query_win32_display_refresh_hz(HWND window) noexcept;

// The OS appearance preferences (issue #187): `SPI_GETCLIENTAREAANIMATION` for reduced motion and
// `AppsUseLightTheme` for the color scheme. It is `Window::appearance()`'s Windows half, so the
// engine reads the same normalized snapshot on every platform; a value the OS cannot answer keeps
// the documented fallback with its assumption bit set (ADR-023 rule 7). Implemented in
// `window_appearance_win.cpp` to keep this TU inside its budget.
[[nodiscard]] AppearancePreference query_win32_appearance() noexcept;

} // namespace tessera::hal::detail
