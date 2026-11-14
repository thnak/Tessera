// SPDX-License-Identifier: MIT
//
// Presentation family selection and support probing for the Vulkan backend (issue #155).
//
// A Linux build can carry both the Xlib and the Wayland WSI: the instance enables both surface
// extensions (an unused one is harmless) and the device then presents through exactly one family.
// Which one is a property of the session and of the HAL window the process owns, so it is resolved
// once at device creation and every later query - queue selection and create_swapchain - consults
// that one answer instead of re-reading the environment.
//
// Rules:
//  - An explicit DeviceDesc::surface wins. Asking for a family the build does not carry resolves to
//    NoPlatform: it is not silently substituted with the other one, because the window handles the
//    caller holds would not match.
//  - Auto resolves like the HAL's create_window(): Wayland first (a Wayland session also publishes
//    DISPLAY for XWayland clients, so WAYLAND_DISPLAY is the more specific fact), then X11, then
//    the only compiled-in family. This keeps every existing caller working unchanged and keeps an
//    Xvfb CI run on X11 exactly as before.
//  - A probe that cannot reach the session (no X display, no compositor) leaves presentation
//    unsupported while the offscreen device stays usable.

// vulkan_internal.hpp pulls in the device definition at the end (vulkan_device_internal.hpp is not
// meant to be included directly).
#include "vulkan_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Independent blocks: both WSI families can be compiled in at once.
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include <wayland-client.h>
#endif

namespace tessera::rendering::vulkan {
namespace {

// MSVC deprecates std::getenv in favour of the bounds-checked _dupenv_s variant (C4996 is fatal
// here), so the Windows arm reads and frees an owned copy; the answer is only "set and non-empty".
bool session_env_set(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
        return false;
    }
    const bool present = buffer[0] != '\0';
    std::free(buffer);
    return present;
#else
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

} // namespace

PresentPlatform VulkanDevice::resolve_present_platform() const {
#if !defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    return PresentPlatform::NoPlatform;
#else
    if (desc_.surface != rhi::SurfacePlatform::Auto) {
        switch (desc_.surface) {
        case rhi::SurfacePlatform::Win32:
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            return PresentPlatform::Win32;
#else
            return PresentPlatform::NoPlatform;
#endif
        case rhi::SurfacePlatform::Xlib:
#if defined(VK_USE_PLATFORM_XLIB_KHR)
            return PresentPlatform::Xlib;
#else
            return PresentPlatform::NoPlatform;
#endif
        case rhi::SurfacePlatform::Wayland:
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
            return PresentPlatform::Wayland;
#else
            return PresentPlatform::NoPlatform;
#endif
        case rhi::SurfacePlatform::Auto:
        case rhi::SurfacePlatform::NoPlatform:
            return PresentPlatform::NoPlatform;
        }
        return PresentPlatform::NoPlatform;
    }

    // Auto: the session decides, in the same order the HAL picks a window backend.
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    if (session_env_set("WAYLAND_DISPLAY")) {
        return PresentPlatform::Wayland;
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    if (session_env_set("DISPLAY")) {
        return PresentPlatform::Xlib;
    }
#endif
    // No session variable: fall back to the single compiled-in family (a Windows build is always
    // Win32; a Linux build without a display variable cannot present anyway).
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    return PresentPlatform::Win32;
#elif defined(VK_USE_PLATFORM_XLIB_KHR) && defined(VK_USE_PLATFORM_WAYLAND_KHR)
    // Both compiled in and no session variable: keep the historical X11 answer, so a CI Xvfb run
    // that created no WAYLAND_DISPLAY keeps presenting through Xlib exactly as before.
    return PresentPlatform::Xlib;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    return PresentPlatform::Xlib;
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    return PresentPlatform::Wayland;
#else
    return PresentPlatform::NoPlatform;
#endif
#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE
}

bool VulkanDevice::has_present_support() const noexcept {
#if !defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    return false;
#else
    switch (present_platform_) {
    case PresentPlatform::Win32:
        return win32_present_supported_;
    case PresentPlatform::Xlib:
        return xlib_present_supported_;
    case PresentPlatform::Wayland:
        return wayland_present_supported_;
    case PresentPlatform::NoPlatform:
        break;
    }
    return false;
#endif
}

void VulkanDevice::probe_present_support(std::uint32_t queue_family) {
#if !defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    (void)queue_family;
#else
    // Only the resolved family is probed: the other compiled-in families exist for other sessions.
    switch (present_platform_) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case PresentPlatform::Win32:
        if (vkGetPhysicalDeviceWin32PresentationSupportKHR != nullptr &&
            vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_device_, queue_family) ==
                VK_TRUE) {
            win32_present_supported_ = true;
        }
        return;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    case PresentPlatform::Xlib: {
        // The Xlib probe needs a Display* and the device keeps no Xlib connection of its own, so it
        // probes with a short-lived one. A missing display leaves presentation unsupported while
        // the offscreen device stays usable.
        Display* probe_display = XOpenDisplay(nullptr);
        if (probe_display == nullptr) {
            return;
        }
        const VisualID probe_visual =
            XVisualIDFromVisual(DefaultVisual(probe_display, DefaultScreen(probe_display)));
        if (vkGetPhysicalDeviceXlibPresentationSupportKHR != nullptr &&
            vkGetPhysicalDeviceXlibPresentationSupportKHR(physical_device_, queue_family,
                                                          probe_display, probe_visual) == VK_TRUE) {
            xlib_present_supported_ = true;
        }
        XCloseDisplay(probe_display);
        return;
    }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    case PresentPlatform::Wayland: {
        // The Wayland probe needs the wl_display* the window will present through. The device must
        // not own a second long-lived connection to the compositor (each display gets its own
        // registry), so it connects for the probe, closes it, and surface creation later uses the
        // HAL's display. A missing compositor leaves presentation unsupported.
        wl_display* probe_display = wl_display_connect(nullptr);
        if (probe_display == nullptr) {
            return;
        }
        if (vkGetPhysicalDeviceWaylandPresentationSupportKHR != nullptr &&
            vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical_device_, queue_family,
                                                             probe_display) == VK_TRUE) {
            wayland_present_supported_ = true;
        }
        wl_display_disconnect(probe_display);
        return;
    }
#endif
    case PresentPlatform::NoPlatform:
        return;
    default:
        // A family the build does not compile WSI headers for: nothing can be probed here.
        return;
    }
#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE
}

} // namespace tessera::rendering::vulkan
