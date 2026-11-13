// SPDX-License-Identifier: MIT
//
// The OS backdrop table of docs/specs/glass-v1.md section 6, driven with a fake HAL: the pure
// resolution rule for every platform row, and the `@media (window-backdrop: none)` fallback that a
// stylesheet uses when the platform offers no material. No window, compositor or GPU is needed.

#include "fake_hal.hpp"
#include "tessera/hal/window_backdrop.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::hal::BackdropCapabilities;
using tessera::hal::BackdropMaterial;
using tessera::hal::BackdropPlatform;
using tessera::hal::BackdropResolution;
using tessera::hal::resolve_window_backdrop;
using tessera::hal::WindowBackdrop;
using tessera::harness::testing::default_fake_metrics;
using tessera::harness::testing::FakeWindow;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::MediaContext;
using tessera::ui::style::Origin;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleNode;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_backdrop: " << message << "\n";
        ++failures;
    }
    return condition;
}

// One row of the section 6 table: the request must resolve to the listed material and the
// translucency state, and the fallback flag must be what the table's "Fallback" column implies.
void expect_resolution(const char* name, WindowBackdrop request, bool transparent,
                       const BackdropCapabilities& capabilities, BackdropMaterial material,
                       bool expected_transparent, bool expected_fallback) {
    const BackdropResolution resolution =
        resolve_window_backdrop(request, transparent, capabilities);
    if (resolution.material != material || resolution.transparent != expected_transparent ||
        resolution.fallback != expected_fallback) {
        std::cerr << "window_backdrop: " << name
                  << ": got material=" << tessera::hal::backdrop_material_name(resolution.material)
                  << " transparent=" << (resolution.transparent ? "true" : "false")
                  << " fallback=" << (resolution.fallback ? "true" : "false") << "\n";
        ++failures;
    }
}

BackdropCapabilities windows11() {
    BackdropCapabilities capabilities;
    capabilities.platform = BackdropPlatform::Windows11;
    capabilities.per_pixel_alpha = true;
    return capabilities;
}

BackdropCapabilities windows10(bool win10_acrylic) {
    BackdropCapabilities capabilities;
    capabilities.platform = BackdropPlatform::Windows10;
    capabilities.win10_acrylic = win10_acrylic;
    capabilities.per_pixel_alpha = true;
    return capabilities;
}

BackdropCapabilities x11(bool kwin) {
    BackdropCapabilities capabilities;
    capabilities.platform = BackdropPlatform::X11;
    capabilities.compositor_blur = kwin;
    capabilities.per_pixel_alpha = true;
    return capabilities;
}

BackdropCapabilities wayland(bool background_effect) {
    BackdropCapabilities capabilities;
    capabilities.platform = BackdropPlatform::Wayland;
    capabilities.compositor_blur = background_effect;
    capabilities.per_pixel_alpha = true;
    return capabilities;
}

std::string computed_background(const MediaContext& media) {
    // The stylesheet pattern of glass-v1.md section 6: an opaque fallback only where the window has
    // no material to show.
    static constexpr std::string_view kCss =
        ".surface { background-color: #000000; }"
        "@media (window-backdrop: none) { .surface { background-color: #202020; } }";
    const auto sheet = compile_stylesheet(kCss, Origin::Author, PropertyRegistry{}, media);
    StyleNode node;
    node.tag = "div";
    node.classes = {"surface"};
    const ComputedStyle style = sheet.compute(node, nullptr);
    const std::string* background = style.find("background-color");
    return background != nullptr ? *background : std::string();
}

} // namespace

int main() {
    // --- Section 6 table: Windows 11 (22H2+) ---------------------------------------------------
    expect_resolution("win11 auto", WindowBackdrop::Auto, false, windows11(),
                      BackdropMaterial::Mica, true, false);
    expect_resolution("win11 mica", WindowBackdrop::Mica, false, windows11(),
                      BackdropMaterial::Mica, true, false);
    expect_resolution("win11 acrylic", WindowBackdrop::Acrylic, false, windows11(),
                      BackdropMaterial::Acrylic, true, false);
    // The table's `blur` column for Windows 11 is Acrylic.
    expect_resolution("win11 blur", WindowBackdrop::Blur, false, windows11(),
                      BackdropMaterial::Acrylic, true, false);
    expect_resolution("win11 none", WindowBackdrop::None, false, windows11(),
                      BackdropMaterial::None, false, false);
    expect_resolution("win11 transparent only", WindowBackdrop::None, true, windows11(),
                      BackdropMaterial::None, true, false);
    // A non-None request implies translucency even without the separate option.
    expect_resolution("win11 mica translucent request", WindowBackdrop::Mica, true, windows11(),
                      BackdropMaterial::Mica, true, false);

    // --- Windows 10: no documented material; acrylic is opt-in ---------------------------------
    expect_resolution("win10 auto", WindowBackdrop::Auto, false, windows10(false),
                      BackdropMaterial::None, true, false);
    expect_resolution("win10 mica", WindowBackdrop::Mica, false, windows10(false),
                      BackdropMaterial::None, true, true);
    expect_resolution("win10 acrylic without opt-in", WindowBackdrop::Acrylic, false,
                      windows10(false), BackdropMaterial::None, true, true);
    expect_resolution("win10 blur without opt-in", WindowBackdrop::Blur, false, windows10(false),
                      BackdropMaterial::None, true, true);
    expect_resolution("win10 acrylic opt-in", WindowBackdrop::Acrylic, false, windows10(true),
                      BackdropMaterial::Acrylic, true, false);
    expect_resolution("win10 blur opt-in", WindowBackdrop::Blur, false, windows10(true),
                      BackdropMaterial::Acrylic, true, false);
    // `auto` never selects the undocumented effect, even when the project enabled it.
    expect_resolution("win10 auto with opt-in", WindowBackdrop::Auto, false, windows10(true),
                      BackdropMaterial::None, true, false);

    // --- Linux X11: blur on KWin only ----------------------------------------------------------
    expect_resolution("x11 kwin auto", WindowBackdrop::Auto, false, x11(true),
                      BackdropMaterial::Blur, true, false);
    expect_resolution("x11 kwin blur", WindowBackdrop::Blur, false, x11(true),
                      BackdropMaterial::Blur, true, false);
    expect_resolution("x11 kwin mica", WindowBackdrop::Mica, false, x11(true),
                      BackdropMaterial::None, true, true);
    expect_resolution("x11 kwin acrylic", WindowBackdrop::Acrylic, false, x11(true),
                      BackdropMaterial::None, true, true);
    expect_resolution("x11 other auto", WindowBackdrop::Auto, false, x11(false),
                      BackdropMaterial::None, true, false);
    expect_resolution("x11 other blur", WindowBackdrop::Blur, false, x11(false),
                      BackdropMaterial::None, true, true);

    // --- Linux Wayland: blur when the compositor offers the protocol ----------------------------
    expect_resolution("wayland offered auto", WindowBackdrop::Auto, false, wayland(true),
                      BackdropMaterial::Blur, true, false);
    expect_resolution("wayland offered blur", WindowBackdrop::Blur, false, wayland(true),
                      BackdropMaterial::Blur, true, false);
    expect_resolution("wayland absent auto", WindowBackdrop::Auto, false, wayland(false),
                      BackdropMaterial::None, true, false);
    expect_resolution("wayland absent blur", WindowBackdrop::Blur, false, wayland(false),
                      BackdropMaterial::None, true, true);

    // --- No material offered, and no per-pixel alpha -------------------------------------------
    BackdropCapabilities nothing;
    nothing.platform = BackdropPlatform::None;
    nothing.per_pixel_alpha = true;
    expect_resolution("no platform auto", WindowBackdrop::Auto, false, nothing,
                      BackdropMaterial::None, true, false);
    BackdropCapabilities opaque = windows11();
    opaque.per_pixel_alpha = false;
    expect_resolution("no per-pixel alpha mica", WindowBackdrop::Mica, false, opaque,
                      BackdropMaterial::None, false, true);
    expect_resolution("no per-pixel alpha transparent", WindowBackdrop::None, true, opaque,
                      BackdropMaterial::None, false, true);

    // Spellings are shared with the media-feature grammar and the diagnostics.
    BackdropMaterial parsed = BackdropMaterial::None;
    expect(tessera::hal::window_backdrop_name(WindowBackdrop::Auto) == "auto", "auto spelling");
    expect(tessera::hal::backdrop_material_name(BackdropMaterial::Acrylic) == "acrylic",
           "acrylic spelling");
    expect(tessera::hal::backdrop_material_from_name("blur", parsed) &&
               parsed == BackdropMaterial::Blur,
           "blur parses");
    expect(!tessera::hal::backdrop_material_from_name("auto", parsed), "auto is not a material");
    // The request grammar (the CSS property and the root inline style) accepts `auto`.
    WindowBackdrop request = WindowBackdrop::None;
    expect(tessera::hal::window_backdrop_from_name("auto", request) &&
               request == WindowBackdrop::Auto,
           "auto parses as a request");
    expect(tessera::hal::window_backdrop_from_name("mica", request) &&
               request == WindowBackdrop::Mica,
           "mica parses as a request");
    expect(!tessera::hal::window_backdrop_from_name("frosted", request),
           "unknown request keyword stays untouched");

    // --- Fake HAL: the resolved metrics drive the @media fallback -------------------------------
    {
        tessera::hal::WindowMetrics metrics = default_fake_metrics();
        FakeWindow window(metrics);
        // The default fake window offers no material: the stylesheet's opaque fallback applies.
        MediaContext no_material;
        no_material.window_backdrop = window.metrics().backdrop;
        expect(no_material.window_backdrop == BackdropMaterial::None,
               "fake window defaults to none");
        expect(computed_background(no_material) == "#202020",
               "fallback applies without a material");

        // A platform that offered Mica reports it through the same WindowMetrics channel.
        metrics.backdrop = BackdropMaterial::Mica;
        metrics.transparent = true;
        window.set_metrics(metrics);
        MediaContext mica;
        mica.window_backdrop = window.metrics().backdrop;
        expect(window.metrics().backdrop == BackdropMaterial::Mica, "fake metrics carry backdrop");
        expect(window.metrics().transparent, "fake metrics carry transparency");
        expect(computed_background(mica) == "#000000", "fallback does not apply with a material");

        // The opaque fallback is reported, not hidden: the fake window can carry the flag the
        // platform sets when the request could not be honored.
        metrics.backdrop = BackdropMaterial::None;
        metrics.transparent = false;
        metrics.backdrop_fallback = true;
        window.set_metrics(metrics);
        expect(window.metrics().backdrop_fallback, "fake metrics carry the fallback flag");
    }

    if (failures != 0) {
        std::cerr << "window_backdrop: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "window_backdrop: ok\n";
    return 0;
}
