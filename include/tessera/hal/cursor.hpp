// SPDX-License-Identifier: MIT

#pragma once

// The pointer-shape seam (issue #191; the normative contract is docs/specs/window-chrome-v1.md
// section 8.5).
//
// A stylesheet's `cursor` property is resolved by `ui::style` into a `CursorKind`, the app maps
// that onto this enum, and the platform maps it onto the native pointer. Nothing here knows a CSS
// keyword, an HWND or an XID: `CursorShape` is the app-visible intent and the platform decides what
// its OS call is (ADR-020).
//
// `Default` is the platform arrow and the initial value of every window. `Hidden` is CSS
// `cursor: none`: the pointer is invisible while this is set; a platform that cannot hide it keeps
// `Default`.

#include <cstdint>
#include <string_view>

namespace tessera::hal {

// One native pointer shape. The set is the CSS cursor keyword set V1 maps (window-chrome-v1.md
// section 8.5); a host that receives a shape it cannot render substitutes its nearest equivalent
// and reports the resolved value in `WindowMetrics::cursor`, never a request it did not honor.
enum class CursorShape : std::uint8_t {
    // The platform's default arrow: CSS `auto` and `default`, and the shape every window starts in.
    Default,
    // CSS `cursor: none`. The pointer is invisible while this is set; a platform that cannot hide
    // it
    // keeps Default.
    Hidden,
    ContextMenu,
    Help,
    Pointer,
    Progress,
    Wait,
    Cell,
    Crosshair,
    Text,
    VerticalText,
    Alias,
    Copy,
    Move,
    NoDrop,
    NotAllowed,
    Grab,
    Grabbing,
    AllScroll,
    ZoomIn,
    ZoomOut,
    ResizeN,
    ResizeE,
    ResizeS,
    ResizeW,
    ResizeNe,
    ResizeNw,
    ResizeSe,
    ResizeSw,
    ResizeNs,
    ResizeEw,
    ResizeNesw,
    ResizeNwse,
    ColResize,
    RowResize,
};

// Number of enumerators in `CursorShape`. A platform's per-shape table is sized with it, so the
// table can never be indexed past its end.
inline constexpr std::uint8_t kCursorShapeCount =
    static_cast<std::uint8_t>(CursorShape::RowResize) + 1;

// The stable lower-case name of a shape ("default", "hidden", "resize-nwse", ...), for the
// deterministic metrics dump and for diagnostics. Never returns nullptr.
[[nodiscard]] std::string_view cursor_shape_name(CursorShape shape) noexcept;

} // namespace tessera::hal
