// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/native_surface_token.hpp"
#include "tessera/hal/window_backdrop.hpp"

#include <cstdint>
#include <string>

namespace tessera::hal {

// One side's inset in PHYSICAL pixels, as a platform reports it before the engine converts to the
// logical pixels `WindowMetrics` carries. Android's `WindowInsets` is the source; the decode rules
// live in the platform backend and are unit-tested there.
struct PhysicalInsets {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// A rectangle in PHYSICAL pixels, window coordinates. Android's `DisplayCutout.getBoundingRects()`
// reports right/bottom as exclusive edges, which the decode converts to x/y/width/height.
struct PhysicalRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

// Opaque native handles for renderer integration; UI/components must keep using WindowMetrics.
// Win32: `window` is the HWND, `display` is unused. X11: `display` is the Display* and `window` the
// Window id. Wayland: `display` is the wl_display* and `window` the wl_surface*. Android: `window`
// is the ANativeWindow* and `display` is unused.
struct NativeWindowHandle {
    void* display = nullptr;
    void* window = nullptr;
};

// Opaque identity of the window's native host element for the platform accessibility root
// (Windows: the HWND that `UiaHostProviderFromHwnd` consumes). UI and accessibility code pass it by
// value and never inspect, cast, or dereference `token`: only a platform/UIA translation unit may
// interpret it, so no OS handle type leaks into a UI-layer type (ADR-020). A null token means the
// backend has no native host element (offscreen/null window or no accessibility platform).
struct AccessibilityHostHandle {
    void* token = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return token != nullptr;
    }
};

// Client-area size is logical pixels; the platform converts it to physical pixels with the
// per-monitor DPI scale. A zero width or height is invalid.
struct WindowDesc {
    std::string title = "Tessera";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool visible = true;
    // The requested OS backdrop (the resolved root `window-backdrop`, glass-v1.md section 6). A
    // non-`Disabled` request implies a translucent client area; `transparent` also asks for one
    // without a named material. `win10_acrylic` opts into the undocumented Windows 10 accent policy
    // that only `win10Acrylic: true` may enable.
    WindowBackdrop backdrop = WindowBackdrop::Disabled;
    bool transparent = false;
    bool win10_acrylic = false;
    // The native surface the caller already owns, for a platform that cannot create a window on its
    // own (Android). Null on every platform that creates its own window, which is the default and
    // what every existing caller keeps. A backend that needs one and does not get it returns
    // nullptr from `create_window`, the documented "no surface yet" answer.
    NativeSurfaceToken surface;
    // The display density the caller measured, 1.0 = mdpi. Only read when `surface` is set: a
    // platform that creates its own window measures the density itself. A non-positive value is
    // treated as 1.0.
    float dpi_scale = 1.0f;
};

} // namespace tessera::hal
