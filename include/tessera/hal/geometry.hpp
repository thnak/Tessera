// SPDX-License-Identifier: MIT

#pragma once

// The two small value types every other HAL window type is built from (docs/design/hal.md
// section 2). They live in their own header so `caption.hpp` and `window.hpp` can both use them
// without an include cycle; `tessera/hal/window.hpp` re-includes this one, so code that includes the
// window seam keeps seeing `WindowInsets` and `WindowRect` unchanged.

#include <cstdint>

namespace tessera::hal {

// Logical-pixel insets owned by the platform (safe area, system bars, notch, IME).
struct WindowInsets {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};

// Axis-aligned rectangle in logical pixels, window coordinates.
struct WindowRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

} // namespace tessera::hal
