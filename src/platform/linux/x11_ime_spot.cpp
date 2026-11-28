// SPDX-License-Identifier: MIT

#include "x11_ime_spot_apply.hpp"

#include <X11/Xlib.h>

namespace tessera::hal::x11 {

void apply_spot_location(XIC context, SpotLocation spot) {
    if (context == nullptr) {
        return;
    }
    XPoint point{spot.x, spot.y};
    // Xlib buffers the request (no round trip). A method that does not honor the attribute for the
    // active input style returns its name; the pre-edit text is still drawn by the application,
    // so a refusal costs only the candidate position and is not an error.
    XVaNestedList attributes = XVaCreateNestedList(0, XNSpotLocation, &point, nullptr);
    if (attributes != nullptr) {
        XSetICValues(context, XNPreeditAttributes, attributes, nullptr);
        XFree(attributes);
    }
}

} // namespace tessera::hal::x11
