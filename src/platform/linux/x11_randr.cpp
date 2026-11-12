// SPDX-License-Identifier: MIT

#include "tessera/hal/refresh_rate.hpp"
#include "x11_randr.hpp"

#include <X11/extensions/Xrandr.h>

namespace tessera::hal::detail {
namespace {

// One CRTC's active mode and its rectangle in root coordinates.
struct CrtcQuery {
    double refresh_hz = kUnknownRefreshHz;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

CrtcQuery query_crtc(Display* display, XRRScreenResources* resources, RRCrtc crtc) {
    CrtcQuery query;
    XRRCrtcInfo* info = XRRGetCrtcInfo(display, resources, crtc);
    if (info == nullptr) {
        return query;
    }
    query.x = info->x;
    query.y = info->y;
    query.width = static_cast<int>(info->width);
    query.height = static_cast<int>(info->height);
    if (info->mode != None) {
        for (int index = 0; index < resources->nmode; ++index) {
            const XRRModeInfo& mode = resources->modes[index];
            if (mode.id != info->mode) {
                continue;
            }
            query.refresh_hz = refresh_hz_from_display_mode(
                static_cast<std::uint64_t>(mode.dotClock), static_cast<std::uint32_t>(mode.hTotal),
                static_cast<std::uint32_t>(mode.vTotal));
            break;
        }
    }
    XRRFreeCrtcInfo(info);
    return query;
}

bool contains(const CrtcQuery& crtc, int x, int y) {
    return crtc.width > 0 && crtc.height > 0 && x >= crtc.x && x < crtc.x + crtc.width &&
           y >= crtc.y && y < crtc.y + crtc.height;
}

} // namespace

double query_x11_display_refresh_hz(Display* display, ::Window window, ::Window root) noexcept {
    if (display == nullptr || window == 0 || root == 0) {
        return kUnknownRefreshHz;
    }
    int event_base = 0;
    int error_base = 0;
    if (XRRQueryExtension(display, &event_base, &error_base) == 0) {
        return kUnknownRefreshHz;
    }
    // XRRGetScreenResources is RandR 1.2; an older server must answer "unknown" rather than raise
    // a protocol error for a request it does not know.
    int major = 0;
    int minor = 0;
    if (XRRQueryVersion(display, &major, &minor) == 0 || (major < 1 || (major == 1 && minor < 2))) {
        return kUnknownRefreshHz;
    }

    XRRScreenResources* resources = XRRGetScreenResources(display, root);
    if (resources == nullptr) {
        return kUnknownRefreshHz;
    }

    // The window's centre selects the CRTC it is on. A reparenting window manager means the
    // window's own origin is not root-relative, so translate instead of reading x/y directly.
    ::Window child = 0;
    int window_x = 0;
    int window_y = 0;
    XWindowAttributes attributes{};
    const bool have_geometry =
        XGetWindowAttributes(display, window, &attributes) != 0 &&
        XTranslateCoordinates(display, window, root, 0, 0, &window_x, &window_y, &child) != 0;
    const int centre_x = window_x + attributes.width / 2;
    const int centre_y = window_y + attributes.height / 2;

    // XRRGetOutputPrimary is RandR 1.3; a 1.2 server must not receive it. Without a primary output
    // the first connected CRTC is the fallback below.
    const RROutput primary_output =
        (major > 1 || minor >= 3) ? XRRGetOutputPrimary(display, root) : None;
    double containing_refresh_hz = kUnknownRefreshHz;
    double primary_refresh_hz = kUnknownRefreshHz;
    double first_refresh_hz = kUnknownRefreshHz;

    for (int index = 0; index < resources->noutput; ++index) {
        XRROutputInfo* output = XRRGetOutputInfo(display, resources, resources->outputs[index]);
        if (output == nullptr) {
            continue;
        }
        if (output->connection == RR_Connected && output->crtc != None) {
            const CrtcQuery query = query_crtc(display, resources, output->crtc);
            if (query.refresh_hz > 0.0) {
                if (first_refresh_hz <= 0.0) {
                    first_refresh_hz = query.refresh_hz;
                }
                if (resources->outputs[index] == primary_output && primary_refresh_hz <= 0.0) {
                    primary_refresh_hz = query.refresh_hz;
                }
                if (have_geometry && containing_refresh_hz <= 0.0 &&
                    contains(query, centre_x, centre_y)) {
                    containing_refresh_hz = query.refresh_hz;
                }
            }
        }
        XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(resources);

    if (containing_refresh_hz > 0.0) {
        return containing_refresh_hz;
    }
    if (primary_refresh_hz > 0.0) {
        return primary_refresh_hz;
    }
    return first_refresh_hz;
}

} // namespace tessera::hal::detail
