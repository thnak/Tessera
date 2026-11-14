// SPDX-License-Identifier: MIT

#pragma once

// Caption hit regions for the opt-in custom title bar (docs/specs/window-chrome-v1.md section 8.1).
//
// The HAL owns the OS non-client hit test: a scene node never sees a `WM_NCHITTEST` result. The
// framework therefore describes *where* its window controls are as a small list of regions in
// logical window pixels, and the platform decides *what* the OS answer for one is. `CaptionRegionKind`
// is the app-visible intent; no `HT*` code, HWND or X11 window crosses this boundary (ADR-020).
//
// The list is empty by default, which is exactly the system-chrome answer: every point is client
// area. Resolution is platform-neutral and pure, so it is unit-testable without a window.

#include "tessera/hal/geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace tessera::hal {

// One interactive region of a custom title bar.
enum class CaptionRegionKind : std::uint8_t {
    // Drag, double-click maximize, drag-to-snap (Windows: HTCAPTION).
    Drag,
    // The window/system menu (Windows: HTSYSMENU); the HAL shows the menu, the app never draws it.
    SystemMenu,
    // Minimise (Windows: HTMINBUTTON).
    Minimize,
    // Maximize/restore (Windows: HTMAXBUTTON; Snap Layouts flies out from it on Windows 11).
    Maximize,
    // Close (Windows: HTCLOSE).
    Close,
};

// A region in logical pixels, window coordinates (origin at the client top-left). A point inside a
// region resolves to that region; a point outside every region is client area.
struct CaptionRegion {
    WindowRect bounds;
    CaptionRegionKind kind = CaptionRegionKind::Drag;
};

// Upper bound of `WindowMetrics::caption_regions`: one system menu, three window controls and a few
// app regions. A longer list is truncated, so the array can never overrun.
inline constexpr std::size_t kMaxCaptionRegions = 8;

// Builds the region list from what a framework title bar reports. At most `kMaxCaptionRegions`
// entries are kept in order (the first winning a point, see `caption_region_at`), and an entry with
// a non-finite coordinate or no area is skipped. Never allocates and never reads past the span.
[[nodiscard]] std::array<CaptionRegion, kMaxCaptionRegions> make_caption_regions(
    std::span<const CaptionRegion> regions, std::uint8_t& count) noexcept;

// The region a point in logical window pixels falls into, or nullptr for client area. Regions are
// tested in list order, so a control drawn on top of the drag strip is listed first. A non-finite
// point is client area: bad input never becomes a caption hit.
[[nodiscard]] const CaptionRegion* caption_region_at(
    std::span<const CaptionRegion> regions, float x, float y) noexcept;

// True when `kind` is a window control whose click the HAL performs (minimize, maximize/restore,
// close) rather than a drag or system-menu region. The framework uses it to decide which regions
// need a hover/pressed state; the platform uses it to route a non-client button press.
[[nodiscard]] constexpr bool is_caption_button(CaptionRegionKind kind) noexcept {
    return kind == CaptionRegionKind::Minimize || kind == CaptionRegionKind::Maximize ||
           kind == CaptionRegionKind::Close;
}

// Lower-case name of a kind ("drag", "system-menu", "minimize", "maximize", "close"), for the
// deterministic metrics dump and for diagnostics. Never returns nullptr.
[[nodiscard]] const char* caption_region_kind_name(CaptionRegionKind kind) noexcept;

} // namespace tessera::hal
