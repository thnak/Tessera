// SPDX-License-Identifier: MIT
//
// RandR monitor and mode queries for the X11 window (docs/specs/os-events-v1.md section 5; ADR-023
// rules 4-7). Split from `x11_display.cpp` so each platform translation unit stays inside the
// file-size budget: that file keeps the DPI/work-area/event rules, this one owns every XRR* call.
//
// Compiled with `TESSERA_X11_RANDR` defined only when the build found libXrandr. Without it the
// callers never reach these functions (they check the guard), so this file would be an empty
// translation unit; CMake only adds it to the target when RandR is found.
//
// Xlib defines `Window` as a macro, so `x11_display.hpp` parses the HAL headers first and this file
// spells `::Window` for the HAL type.

#include "x11_display.hpp"

#include <X11/extensions/Xrandr.h>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace tessera::hal::detail {
namespace {

// RandR's `Bool`; a monitor's `primary` field is one of these, not a C++ bool.
constexpr Bool kPrimary = 1;

DisplayId id_from_name(std::string_view name) noexcept {
    if (name.empty()) {
        return kNoDisplayId;
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    return hash == kNoDisplayId ? 1 : hash;
}

void set_display_name(DisplayInfo& display, std::string_view name) noexcept {
    const std::size_t copied = std::min(name.size(), kMaxDisplayNameBytes - 1);
    if (copied > 0) {
        std::memcpy(display.name.data(), name.data(), copied);
    }
    display.name[copied] = '\0';
}

// The window's centre in root coordinates. A reparenting window manager means the window's own
// origin is not root-relative, so the geometry is translated instead of read directly.
bool window_centre(Display* display, ::Window window, ::Window root, int& centre_x,
                   int& centre_y) noexcept {
    if (display == nullptr || window == 0 || root == 0) {
        return false;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        return false;
    }
    ::Window child = 0;
    int origin_x = 0;
    int origin_y = 0;
    if (XTranslateCoordinates(display, window, root, 0, 0, &origin_x, &origin_y, &child) == 0) {
        return false;
    }
    centre_x = origin_x + attributes.width / 2;
    centre_y = origin_y + attributes.height / 2;
    return true;
}

// The RandR output name (`eDP-1`, `HDMI-1`) that drives `crtc`, or an empty view when none does.
// The name is the display's stable identity across queries.
std::string_view crtc_output_name(Display* display, XRRScreenResources* resources, RRCrtc crtc,
                                  char* buffer, std::size_t capacity) noexcept {
    if (resources == nullptr || crtc == None) {
        return {};
    }
    for (int index = 0; index < resources->noutput; ++index) {
        XRROutputInfo* info = XRRGetOutputInfo(display, resources, resources->outputs[index]);
        if (info == nullptr) {
            continue;
        }
        const bool matches = info->crtc == crtc && info->nameLen > 0;
        std::string_view name;
        if (matches) {
            const std::size_t copied =
                std::min(static_cast<std::size_t>(info->nameLen), capacity - 1);
            std::memcpy(buffer, info->name, copied);
            buffer[copied] = '\0';
            name = std::string_view(buffer, copied);
        }
        XRRFreeOutputInfo(info);
        if (matches) {
            return name;
        }
    }
    return {};
}

// The CRTC a monitor's first output is attached to, or None.
RRCrtc monitor_crtc(Display* display, XRRScreenResources* resources,
                    const XRRMonitorInfo& monitor) noexcept {
    if (resources == nullptr || monitor.noutput <= 0 || monitor.outputs == nullptr) {
        return None;
    }
    XRROutputInfo* info = XRRGetOutputInfo(display, resources, monitor.outputs[0]);
    if (info == nullptr) {
        return None;
    }
    const RRCrtc crtc = info->crtc;
    XRRFreeOutputInfo(info);
    return crtc;
}

double crtc_refresh_hz(Display* display, XRRScreenResources* resources, RRCrtc crtc) noexcept {
    if (resources == nullptr || crtc == None) {
        return kUnknownRefreshHz;
    }
    XRRCrtcInfo* info = XRRGetCrtcInfo(display, resources, crtc);
    if (info == nullptr) {
        return kUnknownRefreshHz;
    }
    double refresh_hz = kUnknownRefreshHz;
    if (info->mode != None) {
        for (int index = 0; index < resources->nmode; ++index) {
            const XRRModeInfo& mode = resources->modes[index];
            if (mode.id != info->mode) {
                continue;
            }
            refresh_hz = refresh_hz_from_display_mode(static_cast<std::uint64_t>(mode.dotClock),
                                                      static_cast<std::uint32_t>(mode.hTotal),
                                                      static_cast<std::uint32_t>(mode.vTotal));
            break;
        }
    }
    XRRFreeCrtcInfo(info);
    return refresh_hz;
}

// The monitor the window's centre is on, else the primary one, else the first: the same precedence
// the refresh query uses, so the two agree about which display the window is on.
const XRRMonitorInfo* choose_monitor(Display* display, ::Window window, ::Window root,
                                     XRRMonitorInfo* monitors, int count) noexcept {
    if (monitors == nullptr || count <= 0) {
        return nullptr;
    }
    int centre_x = 0;
    int centre_y = 0;
    if (window_centre(display, window, root, centre_x, centre_y)) {
        for (int index = 0; index < count; ++index) {
            const XRRMonitorInfo& monitor = monitors[index];
            if (centre_x >= monitor.x && centre_x < monitor.x + monitor.width &&
                centre_y >= monitor.y && centre_y < monitor.y + monitor.height) {
                return &monitors[index];
            }
        }
    }
    for (int index = 0; index < count; ++index) {
        if (monitors[index].primary == kPrimary) {
            return &monitors[index];
        }
    }
    return &monitors[0];
}

} // namespace

bool randr_query_available(Display* display, int* event_base_out) noexcept {
    if (display == nullptr) {
        return false;
    }
    int event_base = 0;
    int error_base = 0;
    if (XRRQueryExtension(display, &event_base, &error_base) == 0) {
        return false;
    }
    // XRRGetMonitors is RandR 1.5; an older server must answer "no" rather than raise a protocol
    // error for a request it does not know.
    int major = 0;
    int minor = 0;
    if (XRRQueryVersion(display, &major, &minor) == 0 || (major < 1 || (major == 1 && minor < 5))) {
        return false;
    }
    if (event_base_out != nullptr) {
        *event_base_out = event_base;
    }
    return true;
}

bool query_x11_active_display(Display* display, ::Window window, ::Window root, DisplayId& id,
                              double& refresh_hz) noexcept {
    id = kNoDisplayId;
    refresh_hz = kUnknownRefreshHz;
    if (display == nullptr || window == 0 || root == 0 || !randr_query_available(display)) {
        return false;
    }
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
    int count = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(display, root, True, &count);
    const XRRMonitorInfo* chosen = choose_monitor(display, window, root, monitors, count);
    bool found = false;
    if (chosen != nullptr && resources != nullptr) {
        const RRCrtc crtc = monitor_crtc(display, resources, *chosen);
        char name_buffer[kMaxDisplayNameBytes]{};
        const std::string_view name =
            crtc_output_name(display, resources, crtc, name_buffer, sizeof(name_buffer));
        if (!name.empty()) {
            id = id_from_name(name);
        }
        refresh_hz = crtc_refresh_hz(display, resources, crtc);
        found = id != kNoDisplayId || refresh_hz > 0.0;
    }
    if (monitors != nullptr) {
        XRRFreeMonitors(monitors);
    }
    if (resources != nullptr) {
        XRRFreeScreenResources(resources);
    }
    return found;
}

std::size_t query_x11_randr_displays(Display* display, ::Window root,
                                     std::span<DisplayInfo> out) noexcept {
    if (out.empty() || display == nullptr || root == 0 || !randr_query_available(display)) {
        return 0;
    }
    int count = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(display, root, True, &count);
    if (monitors == nullptr) {
        return 0;
    }
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
    std::size_t written = 0;
    for (int index = 0; index < count && written < out.size(); ++index) {
        const XRRMonitorInfo& monitor = monitors[index];
        DisplayInfo& info = out[written];
        info = DisplayInfo{};
        const RRCrtc crtc = monitor_crtc(display, resources, monitor);
        char name_buffer[kMaxDisplayNameBytes]{};
        const std::string_view name =
            crtc_output_name(display, resources, crtc, name_buffer, sizeof(name_buffer));
        if (!name.empty()) {
            set_display_name(info, name);
            info.id = id_from_name(name);
        }
        info.primary = monitor.primary == kPrimary;
        info.bounds =
            WindowRect{static_cast<float>(monitor.x), static_cast<float>(monitor.y),
                       static_cast<float>(monitor.width), static_cast<float>(monitor.height)};
        // RandR reports the monitor's physical pixels. The logical extent divides by the desktop
        // scale, which X11 only publishes as `Xft.dpi`; the window already applies that scale to
        // its own metrics, so the raw physical size is what this list reports.
        info.physical_width = static_cast<std::uint32_t>(std::max(monitor.width, 0));
        info.physical_height = static_cast<std::uint32_t>(std::max(monitor.height, 0));
        const double refresh_hz = crtc_refresh_hz(display, resources, crtc);
        if (refresh_hz > 0.0) {
            info.refresh_hz = static_cast<float>(refresh_hz);
            info.refresh_hz_assumed = false;
        }
        ++written;
    }
    if (resources != nullptr) {
        XRRFreeScreenResources(resources);
    }
    XRRFreeMonitors(monitors);
    return written;
}

} // namespace tessera::hal::detail
