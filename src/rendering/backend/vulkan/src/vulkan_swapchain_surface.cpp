// SPDX-License-Identifier: MIT
//
// Presentation-surface creation for the Vulkan backend, one arm per window-system family.
//
// This lives outside vulkan_swapchain.cpp because that file is at its size budget and the family
// dispatch is a self-contained responsibility: it reads the two opaque HAL handles, calls exactly
// one vkCreate*SurfaceKHR, and reports a normalized error. Splitting it also keeps every WSI header
// include in one translation unit, so a build that carries several families does not spread
// platform headers across the swapchain state machine.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "vulkan_swapchain.hpp"

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)

#include <cstdint>

// Independent blocks, not #elif: a Linux build can carry both the Xlib and the Wayland WSI, and
// both headers are needed then.
#if defined(VK_USE_PLATFORM_WIN32_KHR)
#include <windows.h>
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include <wayland-client.h>
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
// VkAndroidSurfaceCreateInfoKHR and vkCreateAndroidSurfaceKHR. ANativeWindow is only forward
// declared there, which is all this file needs: it never dereferences the window.
#include <vulkan/vulkan_android.h>
#endif

namespace tessera::rendering::vulkan {

std::expected<void, rhi::SwapchainError>
VulkanSwapchain::create_surface(const rhi::SwapchainDesc& desc) {
    // The family is fixed at device creation (issue #155). A build can carry Xlib and Wayland at
    // once, so the dispatch is on the resolved family, not on the preprocessor: a chain of
    // `#if/#elif` would make the first compiled-in arm unreachable for the other family.
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    switch (platform_) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case PresentPlatform::Win32: {
        VkWin32SurfaceCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        create_info.hinstance = GetModuleHandleW(nullptr);
        create_info.hwnd = static_cast<HWND>(desc.native_window);
        result = vkCreateWin32SurfaceKHR(instance_, &create_info, nullptr, &surface_);
        break;
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    case PresentPlatform::Xlib: {
        if (desc.native_display == nullptr || desc.native_window == nullptr ||
            vkCreateXlibSurfaceKHR == nullptr) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        VkXlibSurfaceCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        create_info.dpy = static_cast<Display*>(desc.native_display);
        create_info.window =
            static_cast<::Window>(reinterpret_cast<std::uintptr_t>(desc.native_window));
        result = vkCreateXlibSurfaceKHR(instance_, &create_info, nullptr, &surface_);
        break;
    }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    case PresentPlatform::Wayland: {
        if (desc.native_display == nullptr || desc.native_window == nullptr ||
            vkCreateWaylandSurfaceKHR == nullptr) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        // For Wayland the two opaque HAL handles are the compositor connection and the surface the
        // window owns: exactly what VkWaylandSurfaceCreateInfoKHR wants.
        VkWaylandSurfaceCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        create_info.display = static_cast<wl_display*>(desc.native_display);
        create_info.surface = static_cast<wl_surface*>(desc.native_window);
        result = vkCreateWaylandSurfaceKHR(instance_, &create_info, nullptr, &surface_);
        break;
    }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    case PresentPlatform::Android: {
        if (desc.native_window == nullptr || vkCreateAndroidSurfaceKHR == nullptr) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        // Android has no separate display handle: the HAL window's `window` pointer IS the
        // ANativeWindow* the surface is created from, and `native_display` stays null.
        VkAndroidSurfaceCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        create_info.window = static_cast<ANativeWindow*>(desc.native_window);
        result = vkCreateAndroidSurfaceKHR(instance_, &create_info, nullptr, &surface_);
        break;
    }
#endif
    case PresentPlatform::NoPlatform:
        return std::unexpected(rhi::SwapchainError::NotSupported);
    default:
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    if (result != VK_SUCCESS) {
        surface_ = VK_NULL_HANDLE;
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    return {};
}

} // namespace tessera::rendering::vulkan

#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE
