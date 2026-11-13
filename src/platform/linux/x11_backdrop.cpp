// SPDX-License-Identifier: MIT

#include "x11_backdrop.hpp"

#include <X11/Xutil.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace tessera::hal::detail {
namespace {

constexpr long kMaxNameLength = 1024;

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Reads one UTF8_STRING/STRING property of `window` as text. Empty when the property is missing or
// of an unusable type; a window-manager query is startup-only and never on the frame path.
std::string read_text_property(Display* display, ::Window window, Atom property) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, kMaxNameLength, False, AnyPropertyType,
                           &actual_type, &actual_format, &count, &remaining, &data) != Success ||
        data == nullptr || actual_format != 8 || count == 0) {
        if (data != nullptr) {
            XFree(data);
        }
        return {};
    }
    std::string text(reinterpret_cast<const char*>(data), count);
    XFree(data);
    return text;
}

// EWMH `_NET_SUPPORTING_WM_CHECK` names the window manager; the nested `_NET_WM_NAME` is its name.
bool window_manager_is_kwin(Display* display) {
    const Atom check = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", True);
    const Atom wm_name = XInternAtom(display, "_NET_WM_NAME", True);
    if (check == None || wm_name == None) {
        return false;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, RootWindow(display, DefaultScreen(display)), check, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_format, &count, &remaining,
                           &data) != Success ||
        data == nullptr || actual_format != 32 || count == 0) {
        if (data != nullptr) {
            XFree(data);
        }
        return false;
    }
    ::Window manager = 0;
    std::memcpy(&manager, data, sizeof(manager));
    XFree(data);
    if (manager == 0) {
        return false;
    }
    const std::string name = lower(read_text_property(display, manager, wm_name));
    return name.find("kwin") != std::string::npos;
}

bool compositing_manager_running(Display* display, int screen) {
    const std::string selection = "_NET_WM_CM_S" + std::to_string(screen);
    const Atom atom = XInternAtom(display, selection.c_str(), True);
    return atom != None && XGetSelectionOwner(display, atom) != None;
}

} // namespace

X11BackdropSetup prepare_x11_backdrop(Display* display, int screen, bool translucent) noexcept {
    X11BackdropSetup setup;
    if (display == nullptr) {
        return setup;
    }
    setup.capabilities.platform = BackdropPlatform::X11;
    const bool compositor = compositing_manager_running(display, screen);
    setup.capabilities.compositor_blur = compositor && window_manager_is_kwin(display);
    XVisualInfo visual_info{};
    const bool argb_visual = XMatchVisualInfo(display, screen, 32, TrueColor, &visual_info) != 0;
    setup.capabilities.per_pixel_alpha = compositor && argb_visual;
    if (!translucent || !setup.capabilities.per_pixel_alpha) {
        return setup;
    }
    setup.colormap =
        XCreateColormap(display, RootWindow(display, screen), visual_info.visual, AllocNone);
    if (setup.colormap == None) {
        // No colormap means no translucent window: keep the opaque path byte-identical.
        setup.capabilities.per_pixel_alpha = false;
        return setup;
    }
    setup.visual = visual_info.visual;
    setup.depth = 32;
    return setup;
}

BackdropResolution apply_x11_backdrop(Display* display, ::Window window, WindowBackdrop request,
                                      bool transparent,
                                      const BackdropCapabilities& capabilities) noexcept {
    if (display == nullptr || window == 0) {
        BackdropResolution resolution;
        resolution.fallback = request != WindowBackdrop::Disabled || transparent;
        return resolution;
    }
    BackdropResolution resolution = resolve_window_backdrop(request, transparent, capabilities);
    if (!resolution.transparent) {
        return resolution;
    }
    if (resolution.material != BackdropMaterial::Blur) {
        // X11 has no Mica/Acrylic material; the table maps those requests to the opaque fallback
        // (a computed `resolution.fallback`), and a `None` material has nothing to apply.
        return resolution;
    }
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        resolution.material = BackdropMaterial::NoMaterial;
        resolution.fallback = true;
        return resolution;
    }
    // KWin blurs exactly this rectangle; the whole client area is the backdrop of a translucent
    // window, so the app's own translucent background is what tints it.
    const long region[4] = {0, 0, static_cast<long>(std::max(attributes.width, 0)),
                            static_cast<long>(std::max(attributes.height, 0))};
    const Atom blur = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    XChangeProperty(display, window, blur, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(region), 4);
    XFlush(display);
    // The property is advisory: a compositor that ignores it shows the translucent window without
    // blur, which is still the resolved `blur` material (the table's "on KWin" row).
    return resolution;
}

void configure_x11_window_attributes(const X11BackdropSetup& setup,
                                     XSetWindowAttributes& attributes,
                                     unsigned long& mask) noexcept {
    if (setup.visual == nullptr) {
        return;
    }
    attributes.background_pixel = 0;
    attributes.border_pixel = 0;
    attributes.colormap = setup.colormap;
    mask |= CWColormap | CWBorderPixel;
}

void release_x11_backdrop(Display* display, X11BackdropSetup& setup) noexcept {
    if (display != nullptr && setup.colormap != None) {
        XFreeColormap(display, setup.colormap);
    }
    setup.colormap = None;
    setup.visual = nullptr;
    setup.depth = 0;
}

} // namespace tessera::hal::detail
