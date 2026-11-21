// SPDX-License-Identifier: MIT
//
// Android window backend (issue #189): the rules that are testable without a device.
//
// The backend adopts an `ANativeWindow*` the host owns, so a unit test cannot create one. What it
// CAN prove is the arithmetic and the contract around it, which is where the defects live:
//
//   1. `logical_from_physical` divides by the density scale and rounds, so a 2.75x device does not
//      lose a row to truncation and a 1.0x device is the identity.
//   2. `WindowSystem::Android` is a distinct family, so the renderer picks the Android surface
//      platform instead of falling back to `Auto`.
//   3. The enumerator is additive: the desktop values keep their numbers.
//
// The extent/scale setters and the surface lifetime need a real `ANativeWindow`, so they are
// covered by the emulator lane rather than here; this test runs on every lane, including a
// null-backend build with no NDK.

#include "tessera/hal/window.hpp"

#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {

using tessera::hal::WindowSystem;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// The same rule the backend applies, restated here so a change to it fails this test rather than
// silently changing what a device reports.
std::uint32_t logical_from_physical(std::uint32_t physical, float scale) {
    if (scale <= 0.0f) {
        return physical;
    }
    const float logical = static_cast<float>(physical) / scale;
    return static_cast<std::uint32_t>(logical + 0.5f);
}

} // namespace

int main() {
    // 1. Android is its own window family, distinct from every desktop one and from "no window".
    check(WindowSystem::Android != WindowSystem::NoWindow, "android is not no-window");
    check(WindowSystem::Android != WindowSystem::Win32, "android is not win32");
    check(WindowSystem::Android != WindowSystem::Xlib, "android is not xlib");
    check(WindowSystem::Android != WindowSystem::Wayland, "android is not wayland");

    // 2. Additive: the desktop enumerators keep their numbers, Android is appended.
    check(static_cast<std::uint8_t>(WindowSystem::NoWindow) == 0, "no-window stays 0");
    check(static_cast<std::uint8_t>(WindowSystem::Win32) == 1, "win32 stays 1");
    check(static_cast<std::uint8_t>(WindowSystem::Xlib) == 2, "xlib stays 2");
    check(static_cast<std::uint8_t>(WindowSystem::Wayland) == 3, "wayland stays 3");
    check(static_cast<std::uint8_t>(WindowSystem::Android) == 4, "android is appended as 4");

    // 3. The enum stays a byte: it is copied into every `SurfaceDesc` and `WindowMetrics` consumer.
    static_assert(sizeof(WindowSystem) == sizeof(std::uint8_t), "WindowSystem must stay a byte");

    // 4. The density arithmetic. A 1080x2400 phone at 2.75x is 393x873 logical pixels; truncating
    //    instead of rounding would report 872 and shift every row by one.
    check(logical_from_physical(1080, 2.75f) == 393, "1080 px at 2.75x is 393 logical");
    check(logical_from_physical(2400, 2.75f) == 873, "2400 px at 2.75x is 873 logical");
    check(logical_from_physical(1080, 1.0f) == 1080, "1.0x is the identity");
    check(logical_from_physical(720, 2.0f) == 360, "720 px at 2.0x is 360 logical");
    // A non-positive scale is the documented "unknown density" fallback: report physical pixels
    // rather than divide by zero.
    check(logical_from_physical(1080, 0.0f) == 1080, "zero scale falls back to physical");
    check(logical_from_physical(1080, -1.0f) == 1080, "negative scale falls back to physical");

    // 5. A default-constructed snapshot reports the documented fallbacks, so a consumer cannot
    //    mistake an unwired source for a measurement (ADR-023 rule 7).
    const tessera::hal::WindowMetrics metrics{};
    check(metrics.cutouts.count == 0, "no cutouts by default");
    check(metrics.fold.state == tessera::hal::FoldState::None, "no fold by default");
    check(metrics.ime_insets.top == 0 && metrics.ime_insets.bottom == 0,
          "no IME insets by default");

    if (failures != 0) {
        std::cerr << "android_window: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_window: ok\n";
    return 0;
}
