// SPDX-License-Identifier: MIT
//
// Xlib-facing half of the IME spot anchoring (see x11_ime_spot.hpp for the pure mapping).

#pragma once

#include "x11_ime_spot.hpp"

#include <X11/Xlib.h>

namespace tessera::hal::x11 {

// Sets XNSpotLocation on `context`'s pre-edit attributes. Null context is a no-op.
void apply_spot_location(XIC context, SpotLocation spot);

// Maps the caret rect and sends it only when the spot changed (the window keeps one of these).
class SpotAnchor {
public:
    void set(XIC context, const TextInputRect& rect, float scale, int width, int height) {
        SpotLocation spot{};
        if (context != nullptr &&
            tracker_.update(rect, scale, static_cast<std::uint32_t>(width > 0 ? width : 0),
                            static_cast<std::uint32_t>(height > 0 ? height : 0), spot)) {
            apply_spot_location(context, spot);
        }
    }

private:
    SpotTracker tracker_;
};

} // namespace tessera::hal::x11
