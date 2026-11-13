// SPDX-License-Identifier: MIT

#include "window_backdrop.hpp"

#include <cstring>
#include <dwmapi.h>

namespace tessera::hal::detail {
namespace {

constexpr DWORD kWindows11Build = 22621;

// Documented Windows 11 attributes (defined here so an older SDK still compiles).
constexpr DWORD kDwmWindowBackdropType = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr DWORD kDwmBackdropMica = 2;        // DWMSBT_MAINWINDOW
constexpr DWORD kDwmBackdropAcrylic = 3;     // DWMSBT_TRANSIENTWINDOW

// The undocumented Windows 10 accent policy (SetWindowCompositionAttribute). Opt-in only: the
// engine never calls it unless the project asked for `win10Acrylic: true`.
constexpr DWORD kAccentPolicyAttribute = 19; // WCA_ACCENT_POLICY
constexpr DWORD kAccentEnableAcrylicBlurBehind = 4;
// A near-transparent white tint: the accent stays active (alpha 0 disables the material) while the
// visible colour comes from the stylesheet's own background, as in glass-v1.md section 2.
constexpr DWORD kAcrylicGradientColour = 0x01FFFFFF;

struct AccentPolicy {
    DWORD state;
    DWORD flags;
    DWORD gradient_colour;
    DWORD animation_id;
};

struct WindowCompositionAttributeData {
    DWORD attribute;
    void* data;
    SIZE_T size;
};

struct OsVersionInfo {
    ULONG size;
    ULONG major;
    ULONG minor;
    ULONG build;
    ULONG platform_id;
    wchar_t service_pack[128];
};
using RtlGetVersionFn = LONG(WINAPI*)(OsVersionInfo*);
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

// GetProcAddress returns FARPROC, and casting it straight to a function pointer trips C4191 under
// /W4. The size is asserted at build time and the bits are copied, which is the portable way to
// turn a data pointer into a callable one without the warning.
template<typename Function> Function load_proc(HMODULE module, const char* name) noexcept {
    static_assert(sizeof(Function) == sizeof(FARPROC), "function pointer size must match FARPROC");
    const FARPROC proc = module != nullptr ? GetProcAddress(module, name) : nullptr;
    Function function = nullptr;
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

ULONG windows_build_number() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const RtlGetVersionFn rtl_get_version = load_proc<RtlGetVersionFn>(ntdll, "RtlGetVersion");
    if (rtl_get_version == nullptr) {
        return 0;
    }
    OsVersionInfo info{};
    info.size = sizeof(info);
    if (rtl_get_version(&info) != 0) {
        return 0;
    }
    return info.build;
}

bool dwm_composition_enabled() noexcept {
    BOOL enabled = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled != FALSE;
}

// Lets the compositor see the client area's per-pixel alpha instead of an opaque black frame. Both
// the Mica/Acrylic attribute and a plain `transparent: true` window need it.
bool extend_frame_into_client_area(HWND window) noexcept {
    const MARGINS margins{-1, -1, -1, -1};
    return SUCCEEDED(DwmExtendFrameIntoClientArea(window, &margins));
}

bool apply_system_backdrop(HWND window, BackdropMaterial material) noexcept {
    DWORD type = 0;
    switch (material) {
    case BackdropMaterial::Mica:
        type = kDwmBackdropMica;
        break;
    case BackdropMaterial::Acrylic:
        type = kDwmBackdropAcrylic;
        break;
    case BackdropMaterial::NoMaterial:
    case BackdropMaterial::Blur:
        return false;
    }
    return SUCCEEDED(DwmSetWindowAttribute(window, kDwmWindowBackdropType, &type, sizeof(type)));
}

bool apply_accent_acrylic(HWND window) noexcept {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const SetWindowCompositionAttributeFn set_attribute =
        load_proc<SetWindowCompositionAttributeFn>(user32, "SetWindowCompositionAttribute");
    if (set_attribute == nullptr) {
        return false;
    }
    AccentPolicy policy{};
    policy.state = kAccentEnableAcrylicBlurBehind;
    policy.flags = 0;
    policy.gradient_colour = kAcrylicGradientColour;
    WindowCompositionAttributeData data{};
    data.attribute = kAccentPolicyAttribute;
    data.data = &policy;
    data.size = sizeof(policy);
    return set_attribute(window, &data) != FALSE;
}

} // namespace

bool window_backdrop_is_windows_11() noexcept {
    return windows_build_number() >= kWindows11Build;
}

BackdropCapabilities query_backdrop_capabilities(bool win10_acrylic) noexcept {
    BackdropCapabilities capabilities;
    capabilities.platform =
        window_backdrop_is_windows_11() ? BackdropPlatform::Windows11 : BackdropPlatform::Windows10;
    capabilities.win10_acrylic = win10_acrylic;
    // DWM composition is the Windows side of per-pixel alpha; it is on by default on Windows 10
    // and 11, and a machine that disabled it degrades to the opaque fallback.
    capabilities.per_pixel_alpha = dwm_composition_enabled();
    return capabilities;
}

BackdropResolution apply_window_backdrop(HWND window, WindowBackdrop request, bool transparent,
                                         bool win10_acrylic) noexcept {
    if (window == nullptr) {
        BackdropResolution resolution;
        resolution.fallback = request != WindowBackdrop::Disabled || transparent;
        return resolution;
    }
    const BackdropCapabilities capabilities = query_backdrop_capabilities(win10_acrylic);
    BackdropResolution resolution = resolve_window_backdrop(request, transparent, capabilities);
    if (!resolution.transparent) {
        return resolution;
    }
    // Every translucent window needs the frame extension; a material is drawn by DWM behind it.
    if (!extend_frame_into_client_area(window)) {
        BackdropResolution fallback;
        fallback.fallback = request != WindowBackdrop::Disabled || transparent;
        return fallback;
    }
    if (resolution.material == BackdropMaterial::NoMaterial) {
        return resolution;
    }
    const bool applied = capabilities.platform == BackdropPlatform::Windows11
                             ? apply_system_backdrop(window, resolution.material)
                             : apply_accent_acrylic(window);
    if (!applied) {
        // The OS refused the material: the window keeps its translucency and reports the opaque
        // fallback, so the stylesheet's `@media (window-backdrop: none)` rule takes over.
        resolution.material = BackdropMaterial::NoMaterial;
        resolution.fallback = true;
    }
    return resolution;
}

} // namespace tessera::hal::detail
