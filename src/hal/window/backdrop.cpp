// SPDX-License-Identifier: MIT

#include "tessera/hal/window_backdrop.hpp"

namespace tessera::hal {
namespace {

BackdropMaterial resolve_auto(const BackdropCapabilities& capabilities) noexcept {
    switch (capabilities.platform) {
    case BackdropPlatform::Windows11:
        return BackdropMaterial::Mica;
    case BackdropPlatform::Windows10:
        // Windows 10 has no documented material; the opt-in acrylic exists but `auto` never
        // selects an undocumented effect (glass-v1.md section 6, "auto resolves to none").
        return BackdropMaterial::None;
    case BackdropPlatform::X11:
    case BackdropPlatform::Wayland:
        return capabilities.compositor_blur ? BackdropMaterial::Blur : BackdropMaterial::None;
    case BackdropPlatform::None:
        return BackdropMaterial::None;
    }
    return BackdropMaterial::None;
}

BackdropMaterial resolve_explicit(WindowBackdrop request,
                                  const BackdropCapabilities& capabilities) noexcept {
    switch (request) {
    case WindowBackdrop::Mica:
        return capabilities.platform == BackdropPlatform::Windows11 ? BackdropMaterial::Mica
                                                                    : BackdropMaterial::None;
    case WindowBackdrop::Acrylic:
        if (capabilities.platform == BackdropPlatform::Windows11) {
            return BackdropMaterial::Acrylic;
        }
        if (capabilities.platform == BackdropPlatform::Windows10 && capabilities.win10_acrylic) {
            return BackdropMaterial::Acrylic;
        }
        return BackdropMaterial::None;
    case WindowBackdrop::Blur:
        // The Windows rows map `blur` to acrylic (section 6): Windows has no separate blur
        // material.
        switch (capabilities.platform) {
        case BackdropPlatform::Windows11:
            return BackdropMaterial::Acrylic;
        case BackdropPlatform::Windows10:
            return capabilities.win10_acrylic ? BackdropMaterial::Acrylic : BackdropMaterial::None;
        case BackdropPlatform::X11:
        case BackdropPlatform::Wayland:
            return capabilities.compositor_blur ? BackdropMaterial::Blur : BackdropMaterial::None;
        case BackdropPlatform::None:
            return BackdropMaterial::None;
        }
        return BackdropMaterial::None;
    case WindowBackdrop::None:
    case WindowBackdrop::Auto:
        return BackdropMaterial::None;
    }
    return BackdropMaterial::None;
}

} // namespace

BackdropResolution resolve_window_backdrop(WindowBackdrop request, bool transparent,
                                           const BackdropCapabilities& capabilities) noexcept {
    BackdropResolution resolution;
    // The CSS property has no separate transparency flag: asking for a material asks for a
    // translucent client area (glass-v1.md section 2).
    const bool wants_transparency = transparent || request != WindowBackdrop::None;
    if (!wants_transparency) {
        return resolution;
    }
    if (!capabilities.per_pixel_alpha) {
        // No per-pixel alpha: the opaque fallback, with the same diagnostic as an unsupported
        // material. A material behind an opaque surface would never be visible.
        resolution.fallback = true;
        return resolution;
    }
    resolution.transparent = true;
    resolution.material = request == WindowBackdrop::Auto ? resolve_auto(capabilities)
                                                          : resolve_explicit(request, capabilities);
    // `auto` resolving to no material is the documented "no material offered" answer and stays
    // quiet; a named material that the platform refused is the unsupported path.
    if (request != WindowBackdrop::None && request != WindowBackdrop::Auto &&
        resolution.material == BackdropMaterial::None) {
        resolution.fallback = true;
    }
    return resolution;
}

std::string_view backdrop_material_name(BackdropMaterial material) noexcept {
    switch (material) {
    case BackdropMaterial::None:
        return "none";
    case BackdropMaterial::Mica:
        return "mica";
    case BackdropMaterial::Acrylic:
        return "acrylic";
    case BackdropMaterial::Blur:
        return "blur";
    }
    return "none";
}

std::string_view window_backdrop_name(WindowBackdrop request) noexcept {
    switch (request) {
    case WindowBackdrop::None:
        return "none";
    case WindowBackdrop::Auto:
        return "auto";
    case WindowBackdrop::Mica:
        return "mica";
    case WindowBackdrop::Acrylic:
        return "acrylic";
    case WindowBackdrop::Blur:
        return "blur";
    }
    return "none";
}

bool backdrop_material_from_name(std::string_view name, BackdropMaterial& material) noexcept {
    if (name == "none") {
        material = BackdropMaterial::None;
        return true;
    }
    if (name == "mica") {
        material = BackdropMaterial::Mica;
        return true;
    }
    if (name == "acrylic") {
        material = BackdropMaterial::Acrylic;
        return true;
    }
    if (name == "blur") {
        material = BackdropMaterial::Blur;
        return true;
    }
    return false;
}

bool window_backdrop_from_name(std::string_view name, WindowBackdrop& request) noexcept {
    if (name == "auto") {
        request = WindowBackdrop::Auto;
        return true;
    }
    BackdropMaterial material = BackdropMaterial::None;
    if (!backdrop_material_from_name(name, material)) {
        return false;
    }
    switch (material) {
    case BackdropMaterial::None:
        request = WindowBackdrop::None;
        return true;
    case BackdropMaterial::Mica:
        request = WindowBackdrop::Mica;
        return true;
    case BackdropMaterial::Acrylic:
        request = WindowBackdrop::Acrylic;
        return true;
    case BackdropMaterial::Blur:
        request = WindowBackdrop::Blur;
        return true;
    }
    return false;
}

} // namespace tessera::hal
