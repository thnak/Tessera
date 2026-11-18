// SPDX-License-Identifier: MIT
//
// The Linux subpixel sources -> `PixelGeometry` mapping (issue #504, display-aware-rendering.md
// section 2). Pure and display-free: every enumerator of RandR `subpixel_order` and
// `wl_output.subpixel`, every `Xft.rgba` spelling, and an out-of-range value mapping to `Unknown`.

#include "tessera/hal/display.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using tessera::hal::PixelGeometry;

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "subpixel_geometry: " << message << "\n";
        ++failures;
    }
}

void check_randr() {
    // X11/extensions/render.h: SubPixelUnknown 0, HorizontalRGB 1, HorizontalBGR 2, VerticalRGB 3,
    // VerticalBGR 4, None 5.
    expect(tessera::hal::randr_subpixel_geometry(0) == PixelGeometry::Unknown, "randr unknown");
    expect(tessera::hal::randr_subpixel_geometry(1) == PixelGeometry::RgbHorizontal, "randr hrgb");
    expect(tessera::hal::randr_subpixel_geometry(2) == PixelGeometry::BgrHorizontal, "randr hbgr");
    expect(tessera::hal::randr_subpixel_geometry(3) == PixelGeometry::RgbVertical, "randr vrgb");
    expect(tessera::hal::randr_subpixel_geometry(4) == PixelGeometry::BgrVertical, "randr vbgr");
    expect(tessera::hal::randr_subpixel_geometry(5) == PixelGeometry::None, "randr none");
    expect(tessera::hal::randr_subpixel_geometry(6) == PixelGeometry::Unknown, "randr past range");
    expect(tessera::hal::randr_subpixel_geometry(-1) == PixelGeometry::Unknown, "randr negative");
}

void check_wayland() {
    // wl_output.subpixel: unknown 0, none 1, horizontal_rgb 2, horizontal_bgr 3, vertical_rgb 4,
    // vertical_bgr 5.
    expect(tessera::hal::wayland_subpixel_geometry(0) == PixelGeometry::Unknown, "wl unknown");
    expect(tessera::hal::wayland_subpixel_geometry(1) == PixelGeometry::None, "wl none");
    expect(tessera::hal::wayland_subpixel_geometry(2) == PixelGeometry::RgbHorizontal, "wl hrgb");
    expect(tessera::hal::wayland_subpixel_geometry(3) == PixelGeometry::BgrHorizontal, "wl hbgr");
    expect(tessera::hal::wayland_subpixel_geometry(4) == PixelGeometry::RgbVertical, "wl vrgb");
    expect(tessera::hal::wayland_subpixel_geometry(5) == PixelGeometry::BgrVertical, "wl vbgr");
    expect(tessera::hal::wayland_subpixel_geometry(6) == PixelGeometry::Unknown, "wl past range");
    expect(tessera::hal::wayland_subpixel_geometry(0xFFFFFFFFu) == PixelGeometry::Unknown,
           "wl wire garbage");
}

void check_xft() {
    using tessera::hal::xft_rgba_geometry;
    expect(xft_rgba_geometry("rgb") == PixelGeometry::RgbHorizontal, "xft rgb");
    expect(xft_rgba_geometry("bgr") == PixelGeometry::BgrHorizontal, "xft bgr");
    expect(xft_rgba_geometry("vrgb") == PixelGeometry::RgbVertical, "xft vrgb");
    expect(xft_rgba_geometry("vbgr") == PixelGeometry::BgrVertical, "xft vbgr");
    expect(xft_rgba_geometry("none") == PixelGeometry::None, "xft none");
    expect(xft_rgba_geometry("RGB") == PixelGeometry::RgbHorizontal, "xft case-insensitive");
    expect(xft_rgba_geometry("  bgr\n") == PixelGeometry::BgrHorizontal, "xft trims blanks");
    expect(xft_rgba_geometry("") == PixelGeometry::Unknown, "xft empty");
    expect(xft_rgba_geometry("unknown") == PixelGeometry::Unknown, "xft unknown word");
    expect(xft_rgba_geometry("rgbrgbrgb") == PixelGeometry::Unknown, "xft too long");
    expect(xft_rgba_geometry("rg") == PixelGeometry::Unknown, "xft prefix is not a match");
}

void check_resolve() {
    using tessera::hal::resolve_pixel_geometry;
    expect(resolve_pixel_geometry(PixelGeometry::BgrHorizontal, PixelGeometry::RgbHorizontal) ==
               PixelGeometry::BgrHorizontal,
           "a known user preference wins over the panel");
    expect(resolve_pixel_geometry(PixelGeometry::None, PixelGeometry::RgbHorizontal) ==
               PixelGeometry::None,
           "the user turning subpixel off is a known answer");
    expect(resolve_pixel_geometry(PixelGeometry::Unknown, PixelGeometry::RgbHorizontal) ==
               PixelGeometry::RgbHorizontal,
           "no preference falls back to the panel");
    expect(resolve_pixel_geometry(PixelGeometry::Unknown, PixelGeometry::Unknown) ==
               PixelGeometry::Unknown,
           "nothing known stays Unknown");
}

} // namespace

int main() {
    check_randr();
    check_wayland();
    check_xft();
    check_resolve();
    if (failures == 0) {
        std::cout << "subpixel_geometry: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
