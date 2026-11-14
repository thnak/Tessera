// SPDX-License-Identifier: MIT
//
// X11 display-capability reads for one window (docs/specs/os-events-v1.md section 5; ADR-023
// rules 4-7).
//
// This file owns what does not need libXrandr: the desktop `Xft.dpi` read and the pixel/scale
// conversions, the EWMH `_NET_WORKAREA` property, and the normalization of a RandR notification
// into a `WindowEventType`. The RandR monitor/mode queries live in `x11_randr_monitors.cpp`
// (compiled only when libXrandr was found), so each platform translation unit stays inside the
// file-size budget.
//
// Nothing here blocks: every call is an Xlib request answered from the connection, and a missing
// window manager or an old server is an answer ("no"), not an error.
//
// Xlib defines `Window` and `CursorShape` as macros, and `None`/`Bool`/`Status` collide with
// ordinary identifiers. `x11_display.hpp` parses the HAL headers before Xlib for that reason; this
// file spells `::Window` wherever the HAL type is meant.

#include "x11_display.hpp"

#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(TESSERA_X11_RANDR)
#include <X11/extensions/Xrandr.h>
#endif

namespace tessera::hal::detail {

int query_x11_xft_dpi(Display* display) noexcept {
    if (display == nullptr) {
        return kDefaultX11Dpi;
    }
    const char* resources = XResourceManagerString(display);
    if (resources == nullptr || resources[0] == '\0') {
        return kDefaultX11Dpi;
    }
    XrmDatabase database = XrmGetStringDatabase(resources);
    if (database == nullptr) {
        return kDefaultX11Dpi;
    }
    int dpi = kDefaultX11Dpi;
    char* type = nullptr;
    XrmValue value{};
    if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) != 0 &&
        value.addr != nullptr) {
        const char* text = static_cast<const char*>(value.addr);
        char* end = nullptr;
        const double parsed = std::strtod(text, &end);
        if (end != text && std::isfinite(parsed) && parsed > 0.0) {
            dpi = std::clamp(static_cast<int>(std::lround(parsed)), kMinX11Dpi, kMaxX11Dpi);
        }
    }
    XrmDestroyDatabase(database);
    return dpi;
}

float x11_dpi_scale(int dpi) noexcept {
    const int usable = dpi > 0 ? dpi : kDefaultX11Dpi;
    return static_cast<float>(usable) / static_cast<float>(kDefaultX11Dpi);
}

std::uint32_t x11_logical_from_physical(std::uint32_t physical, int dpi) noexcept {
    const int usable = dpi > 0 ? dpi : kDefaultX11Dpi;
    const double scaled = static_cast<double>(physical) * static_cast<double>(kDefaultX11Dpi) /
                          static_cast<double>(usable);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

std::uint32_t x11_physical_from_logical(std::uint32_t logical, int dpi) noexcept {
    const int usable = dpi > 0 ? dpi : kDefaultX11Dpi;
    const double scaled = static_cast<double>(logical) * static_cast<double>(usable) /
                          static_cast<double>(kDefaultX11Dpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

void subscribe_x11_display_events(Display* display, ::Window root) noexcept {
#if defined(TESSERA_X11_RANDR)
    if (display == nullptr || root == 0) {
        return;
    }
    int event_base = 0;
    if (!randr_query_available(display, &event_base)) {
        return;
    }
    // The screen-change mask is what makes RRScreenChangeNotify arrive; the output/CRTC notify
    // masks are what make a hotplug or a mode change observable at all.
    XRRSelectInput(display, root,
                   RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);
#else
    (void)display;
    (void)root;
#endif
}

bool query_x11_work_area(Display* display, ::Window root, int screen, WindowRect& out) noexcept {
    if (display == nullptr || root == 0) {
        return false;
    }
    const Atom work_area = XInternAtom(display, "_NET_WORKAREA", True);
    if (work_area == None) {
        return false;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    // A bounded read: the property is four longs, and a window manager that publishes more is
    // clamped rather than copied.
    if (XGetWindowProperty(display, root, work_area, 0, 16, False, XA_CARDINAL, &actual_type,
                           &actual_format, &count, &remaining, &data) != Success ||
        data == nullptr || actual_format != 32 || count < 4) {
        if (data != nullptr) {
            XFree(data);
        }
        return false;
    }
    const auto* values = reinterpret_cast<const unsigned long*>(data);
    const std::size_t index = screen > 0 && static_cast<std::size_t>(screen) * 4 + 3 < count
                                  ? static_cast<std::size_t>(screen) * 4
                                  : 0;
    out = WindowRect{static_cast<float>(values[index]), static_cast<float>(values[index + 1]),
                     static_cast<float>(values[index + 2]), static_cast<float>(values[index + 3])};
    XFree(data);
    return out.width > 0.0f && out.height > 0.0f;
}

void fill_x11_display_capabilities(Display* display, ::Window window, ::Window root, int screen,
                                   WindowMetrics& metrics) noexcept {
    // Start from the documented fallbacks so every early return leaves a recorded assumption rather
    // than a zero a consumer would read as measured (ADR-023 rule 7).
    metrics.display = kNoDisplayId;
    metrics.refresh_hz = kAssumedDisplayRefreshHz;
    metrics.assumed.refresh_hz = true;
    metrics.flags.vrr = false;
    metrics.assumed.vrr = true;
    metrics.orientation = orientation_from_extent(metrics.logical_width, metrics.logical_height);
    metrics.assumed.orientation = true;
    metrics.work_area = resolve_work_area({}, metrics.logical_width, metrics.logical_height);
    metrics.assumed.work_area = true;

    WindowRect work_area;
    if (query_x11_work_area(display, root, screen, work_area)) {
        // `_NET_WORKAREA` is in root coordinates. A multi-monitor desktop publishes one rectangle
        // per desktop (the whole virtual desktop), which is clamped to the window's logical size.
        metrics.work_area =
            resolve_work_area(work_area, metrics.logical_width, metrics.logical_height);
        metrics.assumed.work_area = false;
    }

    // The RandR half is behind the extension guard: a build without libXrandr keeps the fallbacks
    // above, which is the documented "no display-refresh source" answer.
    DisplayId display_id = kNoDisplayId;
    double refresh_hz = kUnknownRefreshHz;
    if (query_x11_active_display(display, window, root, display_id, refresh_hz)) {
        metrics.display = display_id;
        if (refresh_hz > 0.0) {
            metrics.refresh_hz = static_cast<float>(refresh_hz);
            metrics.assumed.refresh_hz = false;
        }
    }
}

std::size_t query_x11_displays(Display* display, ::Window root,
                               std::span<DisplayInfo> out) noexcept {
    return query_x11_randr_displays(display, root, out);
}

DisplayId x11_display_id(Display* display, ::Window window, ::Window root) noexcept {
    DisplayId id = kNoDisplayId;
    double refresh_hz = kUnknownRefreshHz;
    (void)query_x11_active_display(display, window, root, id, refresh_hz);
    return id;
}

bool x11_event_is_display_change(const XEvent& event) noexcept {
#if defined(TESSERA_X11_RANDR)
    int event_base = 0;
    if (!randr_query_available(event.xany.display, &event_base)) {
        return false;
    }
    return event.type == event_base + RRScreenChangeNotify || event.type == event_base + RRNotify;
#else
    (void)event;
    return false;
#endif
}

WindowEventType x11_display_event_for(const XEvent& event) noexcept {
#if defined(TESSERA_X11_RANDR)
    int event_base = 0;
    if (!randr_query_available(event.xany.display, &event_base)) {
        return WindowEventType::DisplayChanged;
    }
    if (event.type != event_base + RRNotify) {
        // RRScreenChangeNotify: the desktop's size or rotation changed.
        return WindowEventType::DisplayChanged;
    }
    // XRandR events arrive as Xlib *generic* events: `XEvent` has no `xrrnotify` member in
    // this Xlib, so the payload is read through the typed struct (the same object, after
    // `XRRUpdateConfiguration`/`xcb` delivered it).
    const auto* notify_event = reinterpret_cast<const XRRNotifyEvent*>(&event);
    const unsigned long subtype = static_cast<unsigned long>(notify_event->subtype);
    if (subtype == static_cast<unsigned long>(RRNotify_CrtcChange)) {
        return WindowEventType::RefreshRateChanged;
    }
    if (subtype == static_cast<unsigned long>(RRNotify_OutputChange)) {
        // A monitor that became connected was added; one that disconnected was removed. Any other
        // output property change is a plain display change.
        const auto& notify = reinterpret_cast<const XRROutputChangeNotifyEvent&>(event);
        if (notify.connection == RR_Connected) {
            return WindowEventType::DisplayAdded;
        }
        if (notify.connection == RR_Disconnected) {
            return WindowEventType::DisplayRemoved;
        }
        return WindowEventType::DisplayChanged;
    }
    return WindowEventType::DisplayChanged;
#else
    (void)event;
    return WindowEventType::DisplayChanged;
#endif
}

} // namespace tessera::hal::detail
