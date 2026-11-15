// SPDX-License-Identifier: MIT
//
// Win32 display/settings message classification and monitor-device notification registration
// (docs/specs/os-events-v1.md section 4; ADR-023 rules 5 and 6).
//
// A platform callback records an event; it does not lay out, render or block. This file is the
// "record" half for the display family: it turns each OS message into the one normalized event it
// means and registers the monitor device interface, which is what makes a hotplug distinguishable
// from a plain mode change at all. Split from `window_display_win.cpp` (resolution, DPI and refresh
// reads) to keep both under the file-size budget.
//
// Pure classification (no OS state) for every case the window can see, so the mapping is testable
// on any platform: the window tests call it with the same constants Win32 would deliver.

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

namespace tessera::hal::detail {
namespace {

// `GUID_DEVINTERFACE_MONITOR` from `devguid.h`. It is a documented, stable Windows GUID; spelling
// it here keeps this TU free of a `uuid.lib` link dependency the rest of the HAL does not need.
// Value: {E6F07B5F-EE97-4a90-B076-33F5B18B7AE9}.
constexpr GUID kMonitorDeviceInterface{
    0xE6F07B5F, 0xEE97, 0x4A90, {0xB0, 0x76, 0x33, 0xF5, 0xB1, 0x8B, 0x7A, 0xE9}};

} // namespace

DisplayMessage classify_win32_display_message(UINT message, WPARAM wparam) noexcept {
    DisplayMessage result;
    switch (message) {
    case WM_DISPLAYCHANGE:
        // A resolution/mode change on the current display. Without the device notification below a
        // hotplug also lands here; the notification is what distinguishes add from remove.
        result.handled = true;
        result.event = WindowEventType::DisplayChanged;
        result.reason = EventReason::Display;
        return result;
    case WM_DPICHANGED:
        result.handled = true;
        result.event = WindowEventType::DpiChange;
        result.reason = EventReason::Dpi;
        return result;
    case WM_SETTINGCHANGE:
        if (wparam == SPI_SETCLIENTAREAANIMATION) {
            // The system animations toggle: the app re-queries its reduced-motion preference.
            result.handled = true;
            result.forward = true;
            result.event = WindowEventType::SystemSettingsChanged;
            result.reason = EventReason::System;
            return result;
        }
        if (wparam == SPI_SETWORKAREA) {
            // A taskbar/dock moved or resized: the display's usable area is stale.
            result.handled = true;
            result.forward = true;
            result.event = WindowEventType::WorkAreaChanged;
            result.reason = EventReason::System;
            return result;
        }
        return result;
    case WM_DEVICECHANGE:
        // A monitor was attached or detached (the monitor interface class is registered by
        // `register_win32_display_notifications`). A generic volume/port notification is not a
        // display change, so it is recorded as the generic type rather than a hotplug.
        result.handled = true;
        result.forward = true;
        if (wparam == DBT_DEVICEARRIVAL) {
            result.event = WindowEventType::DisplayAdded;
            result.reason = EventReason::Hotplug;
        } else if (wparam == DBT_DEVICEREMOVECOMPLETE) {
            result.event = WindowEventType::DisplayRemoved;
            result.reason = EventReason::Hotplug;
        } else {
            result.event = WindowEventType::DisplayChanged;
            result.reason = EventReason::System;
        }
        return result;
    default:
        return result;
    }
}

HDEVNOTIFY register_win32_display_notifications(HWND window) noexcept {
    if (window == nullptr) {
        return nullptr;
    }
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    // `GUID_DEVINTERFACE_MONITOR`: the monitor device class the OS event table names
    // (docs/specs/os-events-v1.md section 4, "Monitor/device add/remove").
    filter.dbcc_classguid = kMonitorDeviceInterface;
    return RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void unregister_win32_display_notifications(HDEVNOTIFY notify) noexcept {
    if (notify != nullptr) {
        UnregisterDeviceNotification(notify);
    }
}

} // namespace tessera::hal::detail
