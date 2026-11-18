// SPDX-License-Identifier: MIT
//
// X11 subpixel geometry (issue #504, display-aware-rendering.md section 2): the user's `Xft.rgba`
// resource reaches `WindowMetrics::pixel_geometry` and `DisplayInfo::pixel_geometry`.
//
// Headless: the test publishes the resource itself (the property `xrdb` writes) on a bare Xvfb and
// creates a window per case, so it needs no panel. `XRROutputInfo::subpixel_order` cannot be set by
// a client and Xvfb reports it unknown, so the per-output value is covered by the pure mapper test
// (subpixel_geometry); this is the end-to-end path that a headless server can drive.
//
// Skips with a diagnostic when no X server is reachable.

#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
// X.h defines `CursorShape` as an object-like macro that would rewrite the HAL enumerator names.
#ifdef CursorShape
#undef CursorShape
#endif
// X.h also defines `None`, which collides with `PixelGeometry::None`.
#ifdef None
#undef None
#endif
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using tessera::hal::PixelGeometry;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "window_x11_subpixel: " << message << "\n";
        ++failures;
    }
}

// Replaces (or, for an empty string, deletes) the RESOURCE_MANAGER property of the root window,
// which is what `XResourceManagerString` reads for a connection opened afterwards.
void publish_resources(Display* display, const std::string& resources) {
    const ::Window root = DefaultRootWindow(display);
    const Atom property = XInternAtom(display, "RESOURCE_MANAGER", False);
    if (resources.empty()) {
        XDeleteProperty(display, root, property);
    } else {
        XChangeProperty(display, root, property, XA_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(resources.data()),
                        static_cast<int>(resources.size()));
    }
    XSync(display, False);
}

void check_case(Display* control, const std::string& resources, PixelGeometry expected,
                const char* label) {
    publish_resources(control, resources);

    tessera::hal::WindowDesc desc;
    desc.title = std::string("Tessera X11 subpixel ") + label;
    desc.width = 160;
    desc.height = 120;
    desc.visible = false;
    auto window = tessera::hal::create_window(desc);
    if (window == nullptr) {
        expect(false, std::string(label) + ": create_window returns a window");
        return;
    }
    expect(window->metrics().pixel_geometry == expected,
           std::string(label) + ": WindowMetrics::pixel_geometry");

    // Every listed output carries the same preference, because Xft.rgba is desktop-wide.
    std::array<tessera::hal::DisplayInfo, 8> displays{};
    const std::size_t count = window->displays(displays);
    for (std::size_t index = 0; index < count; ++index) {
        expect(displays[index].pixel_geometry == expected,
               std::string(label) + ": DisplayInfo::pixel_geometry");
    }
}

} // namespace

int main() {
    const char* display_name = std::getenv("DISPLAY");
    if (display_name == nullptr || display_name[0] == '\0') {
        std::cout << "window_x11_subpixel: skipped (DISPLAY is not set)\n";
        return 0;
    }
    setenv("TESSERA_WINDOW_BACKEND", "x11", 1);
    Display* control = XOpenDisplay(nullptr);
    if (control == nullptr) {
        std::cout << "window_x11_subpixel: skipped (cannot open DISPLAY)\n";
        return 0;
    }

    check_case(control, "Xft.rgba: rgb\n", PixelGeometry::RgbHorizontal, "rgb");
    check_case(control, "Xft.rgba: bgr\n", PixelGeometry::BgrHorizontal, "bgr");
    check_case(control, "Xft.rgba: vrgb\n", PixelGeometry::RgbVertical, "vrgb");
    check_case(control, "Xft.dpi: 96\nXft.rgba:\tNONE\n", PixelGeometry::None, "none");
    // No preference and an unrecognised value both fall through to the panel, which Xvfb reports
    // as unknown: never a guessed stripe.
    check_case(control, "Xft.rgba: sideways\n", PixelGeometry::Unknown, "unrecognised");
    check_case(control, "", PixelGeometry::Unknown, "absent");

    publish_resources(control, "");
    XCloseDisplay(control);
    if (failures == 0) {
        std::cout << "window_x11_subpixel: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
