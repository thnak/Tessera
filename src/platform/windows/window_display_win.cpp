// SPDX-License-Identifier: MIT
//
// Win32 display-capability reads (docs/specs/os-events-v1.md section 4; ADR-023 rules 4-7).
//
// Every read reports what the OS actually answered: a monitor the API cannot describe, a mode with
// no usable refresh, or a DPI the window does not have all fall through to the documented fallback
// and set the matching `*_assumed` flag. Nothing here guesses, blocks, or throws; the display
// topology is a handful of `GetMonitorInfoW` calls, so the window's own thread runs it at a tick
// boundary when an event says the list is stale.

// The Win32 headers define `min`/`max` as macros unless NOMINMAX is set before them; this TU uses
// std::max/std::min, so the guard has to come first (window.cpp does the same).
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

#include <ShellScalingApi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace tessera::hal::detail {

std::wstring win32_widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        // Titles are display-only; fall back to the process code page instead of dropping them.
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()),
                                     nullptr, 0);
    }
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

namespace {
// A stable id from the monitor's GDI device name. FNV-1a over the name: the same monitor answers
// the same id across queries (which is what makes a DisplayAdded/DisplayRemoved pair name one
// display) without this build keeping a table of HMONITORs.
DisplayId id_from_name(std::string_view name) noexcept {
    if (name.empty()) {
        return kNoDisplayId;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    // 0 is the "no display" value, so a name that happens to hash to 0 takes 1 instead.
    return hash == kNoDisplayId ? 1 : hash;
}

// The GDI device name is ASCII (`\\.\DISPLAY1`); a bounded byte copy avoids a code-page call for a
// name this build only uses as an opaque identity and a diagnostics label. Returns a view of
// `buffer`, whose size must be `kMaxDisplayNameBytes`.
std::string_view narrow_device_name(const WCHAR (&wide)[CCHDEVICENAME], char* buffer,
                                    std::size_t capacity) noexcept {
    std::size_t length = 0;
    while (length < CCHDEVICENAME && wide[length] != L'\0') {
        ++length;
    }
    const std::size_t copied = std::min(length, capacity - 1);
    for (std::size_t i = 0; i < copied; ++i) {
        const WCHAR value = wide[i];
        buffer[i] = value < 0x80 ? static_cast<char>(value) : '?';
    }
    buffer[copied] = '\0';
    return std::string_view(buffer, copied);
}

// Widens a narrow device name for the DEVMODE API. A GDI name is ASCII by construction; a longer
// name (or one with a non-ASCII byte) is truncated to what the fixed buffer holds, which makes the
// subsequent query fail and the caller fall back rather than read out of bounds.
void widen_device_name(std::string_view name, WCHAR (&wide)[CCHDEVICENAME]) noexcept {
    const std::size_t copied = std::min(name.size(), static_cast<std::size_t>(CCHDEVICENAME - 1));
    for (std::size_t i = 0; i < copied; ++i) {
        wide[i] = static_cast<WCHAR>(name[i]);
    }
}

bool fill_monitor(HMONITOR monitor, bool primary, DisplayInfo& display) noexcept {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return false;
    }
    display = DisplayInfo{};
    char name_buffer[kMaxDisplayNameBytes]{};
    const std::string_view device =
        narrow_device_name(info.szDevice, name_buffer, sizeof(name_buffer));
    display.id = id_from_name(device);
    const std::size_t copied = std::min(device.size(), kMaxDisplayNameBytes - 1);
    if (copied > 0) {
        std::memcpy(display.name.data(), device.data(), copied);
    }
    display.name[copied] = '\0';
    display.primary = primary;

    const RECT& rect = info.rcMonitor;
    display.bounds = WindowRect{static_cast<float>(rect.left), static_cast<float>(rect.top),
                                static_cast<float>(rect.right - rect.left),
                                static_cast<float>(rect.bottom - rect.top)};

    // The monitor's own DPI: `GetDpiForMonitor` is the per-monitor answer and needs no window.
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        display.scale = display_scale_from_dpi(static_cast<float>(dpi_x));
        display.scale_assumed = false;
    }

    DEVMODEW mode{};
    mode.dmSize = static_cast<WORD>(sizeof(mode));
    WCHAR wide[CCHDEVICENAME]{};
    widen_device_name(device, wide);
    if (EnumDisplaySettingsExW(wide, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE) {
        display.physical_width = static_cast<std::uint32_t>(std::max<LONG>(mode.dmPelsWidth, 0));
        display.physical_height = static_cast<std::uint32_t>(std::max<LONG>(mode.dmPelsHeight, 0));
        // DEVMODE documents 0 ("hardware default") and 1 ("default") as sentinels, never a rate;
        // the pure sanitizer accepts 1 Hz, so both are rejected here.
        if (mode.dmDisplayFrequency > 1) {
            const double refresh_hz =
                sanitize_display_refresh_hz(static_cast<double>(mode.dmDisplayFrequency));
            if (refresh_hz > 0.0) {
                display.refresh_hz = static_cast<float>(refresh_hz);
                display.refresh_hz_assumed = false;
            }
        }
    }
    return true;
}

struct EnumerationContext {
    std::span<DisplayInfo> out;
    std::size_t count = 0;
    HMONITOR primary = nullptr;
};

} // namespace

DisplayId win32_display_id(HMONITOR monitor) noexcept {
    if (monitor == nullptr) {
        return kNoDisplayId;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return kNoDisplayId;
    }
    char name_buffer[kMaxDisplayNameBytes]{};
    return id_from_name(narrow_device_name(info.szDevice, name_buffer, sizeof(name_buffer)));
}

void fill_win32_display_capabilities(HWND window, WindowMetrics& metrics) noexcept {
    // Start from the documented fallbacks so every early return leaves a consistent, recorded
    // assumption rather than a zero that a consumer would read as measured.
    metrics.display = kNoDisplayId;
    metrics.refresh_hz = kAssumedDisplayRefreshHz;
    metrics.assumed.refresh_hz = true;
    metrics.flags.vrr = false;
    metrics.assumed.vrr = true;
    metrics.work_area = resolve_work_area({}, metrics.logical_width, metrics.logical_height);
    metrics.assumed.work_area = true;
    metrics.orientation = orientation_from_extent(metrics.logical_width, metrics.logical_height);
    metrics.assumed.orientation = true;

    if (window == nullptr) {
        return;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return;
    }
    DisplayInfo display;
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!fill_monitor(monitor, monitor == primary, display)) {
        return;
    }
    metrics.display = display.id;
    metrics.refresh_hz = display.refresh_hz;
    metrics.assumed.refresh_hz = display.refresh_hz_assumed;

    // The work area is in virtual-screen physical pixels; the metrics are logical pixels, so it is
    // divided by the same scale the size uses. `GetMonitorInfoW` already excludes the taskbar.
    const double scale = std::isfinite(metrics.dpi_scale) && metrics.dpi_scale > 0.0f
                             ? static_cast<double>(metrics.dpi_scale)
                             : 1.0;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != FALSE) {
        const RECT& work = info.rcWork;
        metrics.work_area = resolve_work_area(
            WindowRect{static_cast<float>(static_cast<double>(work.left) / scale),
                       static_cast<float>(static_cast<double>(work.top) / scale),
                       static_cast<float>(static_cast<double>(work.right - work.left) / scale),
                       static_cast<float>(static_cast<double>(work.bottom - work.top) / scale)},
            metrics.logical_width, metrics.logical_height);
        metrics.assumed.work_area = false;
    }

    // A rotated monitor is the one case the OS states orientation directly; otherwise the window's
    // logical extent is the honest answer and stays marked as derived.
    DEVMODEW mode{};
    mode.dmSize = static_cast<WORD>(sizeof(mode));
    WCHAR wide[CCHDEVICENAME]{};
    widen_device_name(display_info_name(display), wide);
    if (EnumDisplaySettingsExW(wide, ENUM_CURRENT_SETTINGS, &mode, 0) != FALSE &&
        (mode.dmFields & DM_DISPLAYORIENTATION) != 0) {
        const bool portrait =
            mode.dmDisplayOrientation == DMDO_90 || mode.dmDisplayOrientation == DMDO_270;
        metrics.orientation = portrait ? Orientation::Portrait : Orientation::Landscape;
        metrics.assumed.orientation = false;
    }
}

std::size_t query_win32_displays(std::span<DisplayInfo> out) noexcept {
    if (out.empty()) {
        return 0;
    }
    EnumerationContext context{out, 0, MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)};
    // The callback runs on this thread inside the enumeration; it only fills the caller's array, so
    // no user code and no allocation happens here.
    const auto callback = [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<EnumerationContext*>(data);
        if (state.count >= state.out.size()) {
            return FALSE;
        }
        if (fill_monitor(monitor, monitor == state.primary, state.out[state.count])) {
            ++state.count;
        }
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, callback, reinterpret_cast<LPARAM>(&context));
    return context.count;
}

double query_win32_display_refresh_hz(HWND window) noexcept {
    if (window == nullptr) {
        return kUnknownRefreshHz;
    }
    // The monitor the window is on, not the primary one: a multi-monitor desktop can mix rates and
    // the present cap must follow the display the user actually sees. The display-mode API is
    // chosen over DwmGetCompositionTimingInfo because the latter describes the compositor's timing
    // for the whole desktop (and needs an active composition), while EnumDisplaySettingsExW reports
    // this monitor's current mode.
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return kUnknownRefreshHz;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return kUnknownRefreshHz;
    }
    char name_buffer[kMaxDisplayNameBytes]{};
    const std::string_view device =
        narrow_device_name(info.szDevice, name_buffer, sizeof(name_buffer));
    WCHAR wide[CCHDEVICENAME]{};
    widen_device_name(device, wide);
    DEVMODEW mode{};
    mode.dmSize = static_cast<WORD>(sizeof(mode));
    if (EnumDisplaySettingsExW(wide, ENUM_CURRENT_SETTINGS, &mode, 0) == FALSE) {
        return kUnknownRefreshHz;
    }
    // DEVMODE documents 0 ("hardware default") and 1 ("default") as frequency sentinels, never as
    // a real rate; the pure sanitizer would accept 1 Hz, so reject both here.
    if (mode.dmDisplayFrequency <= 1) {
        return kUnknownRefreshHz;
    }
    return sanitize_display_refresh_hz(static_cast<double>(mode.dmDisplayFrequency));
}

} // namespace tessera::hal::detail
