// SPDX-License-Identifier: MIT
//
// Android presentation family (VK_KHR_android_surface). The interesting rules are the ones that are
// easy to get wrong and impossible to see on a desktop lane:
//
//   1. `SurfacePlatform::Android` is a distinct enumerator, so a caller that knows it presents to
//      an ANativeWindow never falls back to a desktop family.
//   2. Android has no `vkGetPhysicalDeviceAndroidPresentationSupportKHR`, so presentation support
//      is the queue family's graphics capability rather than a driver query. A build that assumed
//      the desktop probe shape would report "no presentation" on every Android device.
//   3. The enumerator is additive: the desktop values keep their numbers, because they are part of
//      the RHI 1.4 surface contract (docs/specs/versioning-v1.md).
//
// The Vulkan headers are not available to this test on every lane (a null-backend build has no
// Vulkan SDK), so the assertions are on the public RHI enum and on the plain-data policy, not on a
// live device.

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {

using tessera::rendering::rhi::SurfacePlatform;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // 1. Android is its own family, distinct from every desktop one and from "no platform".
    check(SurfacePlatform::Android != SurfacePlatform::NoPlatform, "android is not no-platform");
    check(SurfacePlatform::Android != SurfacePlatform::Win32, "android is not win32");
    check(SurfacePlatform::Android != SurfacePlatform::Xlib, "android is not xlib");
    check(SurfacePlatform::Android != SurfacePlatform::Wayland, "android is not wayland");
    check(SurfacePlatform::Android != SurfacePlatform::Auto, "android is not auto");

    // 2. The enumerator is additive: the RHI 1.4 values keep their numbers, so a serialized or
    //    compiled-against-1.4 caller still means the same family. Android is appended after them.
    check(static_cast<std::uint8_t>(SurfacePlatform::Auto) == 0, "auto stays 0");
    check(static_cast<std::uint8_t>(SurfacePlatform::NoPlatform) == 1, "no-platform stays 1");
    check(static_cast<std::uint8_t>(SurfacePlatform::Win32) == 2, "win32 stays 2");
    check(static_cast<std::uint8_t>(SurfacePlatform::Xlib) == 3, "xlib stays 3");
    check(static_cast<std::uint8_t>(SurfacePlatform::Wayland) == 4, "wayland stays 4");
    check(static_cast<std::uint8_t>(SurfacePlatform::Android) == 5, "android is appended as 5");

    // 3. The enum stays a byte: `DeviceDesc` is copied into every device creation and the RHI keeps
    //    its value types small (docs/guides/code-style.md).
    static_assert(sizeof(SurfacePlatform) == sizeof(std::uint8_t),
                  "SurfacePlatform must stay a byte");

    // 4. `DeviceDesc` defaults to Auto, so an existing caller that never named a family keeps
    //    working and the backend resolves it from the build target and the session.
    const tessera::rendering::rhi::DeviceDesc desc{};
    check(desc.surface == SurfacePlatform::Auto, "DeviceDesc defaults to auto");

    if (failures != 0) {
        std::cerr << "android_surface_platform: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_surface_platform: ok\n";
    return 0;
}
