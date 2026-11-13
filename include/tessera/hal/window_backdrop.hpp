// SPDX-License-Identifier: MIT
//
// The OS backdrop value seam (docs/specs/glass-v1.md section 6).
//
// A platform reports what its window system offers as raw facts (`BackdropCapabilities`).
// Everything that decides what those facts *mean* - which material `auto` resolves to, whether an
// explicit request has to fall back, and whether a translucent window is possible at all - lives in
// these pure functions, so the normative table of section 6 is testable without a window, a
// compositor or an OS call. The platform code stays thin: detect the capabilities, apply the
// returned material, report the result through `WindowMetrics::backdrop`.

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::hal {

// The material the operating system draws behind a translucent window. `None` means the platform
// draws no material: the window is either opaque, or translucent over the desktop only.
enum class BackdropMaterial : std::uint8_t {
    None,
    Mica,
    Acrylic,
    Blur,
};

// What an app asks for: the resolved value of the root element's `window-backdrop` CSS property, or
// the matching `WindowOptions`/`WindowDesc` field. `Auto` picks the best material the platform has
// (glass-v1.md section 6); it is never reported as a resolved material.
enum class WindowBackdrop : std::uint8_t {
    None,
    Auto,
    Mica,
    Acrylic,
    Blur,
};

// The window-system family the capabilities describe. The distinction between Windows 11 and
// Windows 10 matters because `auto` resolves to Mica on 11 and to no material on 10 (section 6).
enum class BackdropPlatform : std::uint8_t {
    None, // Headless/null HAL, or a window system with no backdrop support.
    Windows11,
    Windows10,
    X11,
    Wayland,
};

// Raw window-system facts a platform backend detected. Nothing here is a decision: `resolve_*`
// turns these into the resolved material, so a fake HAL can drive every row of the section 6 table.
struct BackdropCapabilities {
    BackdropPlatform platform = BackdropPlatform::None;
    // The compositor offers a blur-behind effect: Wayland `ext-background-effect-v1` when the
    // compositor advertises it, or KWin's `_KDE_NET_WM_BLUR_BEHIND_REGION` under X11. Unused on
    // Windows, where the platform family implies the available materials.
    bool compositor_blur = false;
    // The Windows 10 opt-in for the undocumented `SetWindowCompositionAttribute` acrylic
    // (`win10Acrylic: true`). Ignored on every other platform.
    bool win10_acrylic = false;
    // The window system can present per-pixel alpha at all: DWM composition is on, an X11
    // compositing manager is running and a 32-bit visual is available, or the surface is a Wayland
    // surface. Without it translucency degrades to opaque before any material is chosen, so an OS
    // material would be invisible.
    bool per_pixel_alpha = false;
};

// One resolution of a request against a platform's capabilities.
struct BackdropResolution {
    // What `WindowMetrics::backdrop` reports.
    BackdropMaterial material = BackdropMaterial::None;
    // Whether the window itself is translucent. False is the opaque fallback of section 6.
    bool transparent = false;
    // True when the app asked for translucency or for a material the platform cannot provide, so
    // the engine reports TESSERA_GLASS_WINDOW_UNSUPPORTED once and the opaque fallback applies.
    bool fallback = false;
};

// The normative resolution of glass-v1.md section 6. `transparent` is the `Window` option;
// a non-`None` `request` implies translucency even when the option is false, because the CSS
// property has no separate transparency flag. `Auto` resolving to `None` is a successful answer
// (no material is offered), not a fallback; an explicit material that cannot be honored and a
// translucent window that cannot be presented are the two fallback cases.
[[nodiscard]] BackdropResolution
resolve_window_backdrop(WindowBackdrop request, bool transparent,
                        const BackdropCapabilities& capabilities) noexcept;

// Stable lowercase spellings for diagnostics, logs and the `@media (window-backdrop: ...)` value
// grammar. `None` is spelled "none" so the media query and the CSS value share one vocabulary.
[[nodiscard]] std::string_view backdrop_material_name(BackdropMaterial material) noexcept;
[[nodiscard]] std::string_view window_backdrop_name(WindowBackdrop request) noexcept;

// Parses one `window-backdrop` / media-feature keyword. Unknown spellings (including "auto", which
// is a request and never a resolved material) leave `material` untouched and return false.
[[nodiscard]] bool backdrop_material_from_name(std::string_view name,
                                               BackdropMaterial& material) noexcept;

// Parses one `window-backdrop` request keyword (`none`, `auto`, `mica`, `acrylic`, `blur`), the
// grammar of the CSS property. An unknown spelling leaves `request` untouched and returns false, so
// a caller can keep the documented default instead of guessing.
[[nodiscard]] bool window_backdrop_from_name(std::string_view name,
                                             WindowBackdrop& request) noexcept;

} // namespace tessera::hal
