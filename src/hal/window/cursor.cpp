// SPDX-License-Identifier: MIT
//
// The pointer-shape values the HAL exposes (issue #191). Pure and platform-neutral: no OS header,
// so the metrics dump and the app's mapping are testable without a window or a display.

#include "tessera/hal/cursor.hpp"

#include <array>
#include <cstddef>

namespace tessera::hal {
namespace {

struct ShapeName {
    CursorShape shape;
    std::string_view name;
};

// One row per enumerator, in `CursorShape` declaration order. The names are the CSS keywords where
// one exists (so the diagnostic text a user reads matches the stylesheet), with `default`/`hidden`
// standing for `auto`/`none`.
constexpr std::array<ShapeName, kCursorShapeCount> kShapeNames{{
    {CursorShape::Default, "default"},
    {CursorShape::Hidden, "hidden"},
    {CursorShape::ContextMenu, "context-menu"},
    {CursorShape::Help, "help"},
    {CursorShape::Pointer, "pointer"},
    {CursorShape::Progress, "progress"},
    {CursorShape::Wait, "wait"},
    {CursorShape::Cell, "cell"},
    {CursorShape::Crosshair, "crosshair"},
    {CursorShape::Text, "text"},
    {CursorShape::VerticalText, "vertical-text"},
    {CursorShape::Alias, "alias"},
    {CursorShape::Copy, "copy"},
    {CursorShape::Move, "move"},
    {CursorShape::NoDrop, "no-drop"},
    {CursorShape::NotAllowed, "not-allowed"},
    {CursorShape::Grab, "grab"},
    {CursorShape::Grabbing, "grabbing"},
    {CursorShape::AllScroll, "all-scroll"},
    {CursorShape::ZoomIn, "zoom-in"},
    {CursorShape::ZoomOut, "zoom-out"},
    {CursorShape::ResizeN, "n-resize"},
    {CursorShape::ResizeE, "e-resize"},
    {CursorShape::ResizeS, "s-resize"},
    {CursorShape::ResizeW, "w-resize"},
    {CursorShape::ResizeNe, "ne-resize"},
    {CursorShape::ResizeNw, "nw-resize"},
    {CursorShape::ResizeSe, "se-resize"},
    {CursorShape::ResizeSw, "sw-resize"},
    {CursorShape::ResizeNs, "ns-resize"},
    {CursorShape::ResizeEw, "ew-resize"},
    {CursorShape::ResizeNesw, "nesw-resize"},
    {CursorShape::ResizeNwse, "nwse-resize"},
    {CursorShape::ColResize, "col-resize"},
    {CursorShape::RowResize, "row-resize"},
}};

static_assert(kShapeNames.size() == kCursorShapeCount,
              "kShapeNames must list every CursorShape in declaration order");

} // namespace

std::string_view cursor_shape_name(CursorShape shape) noexcept {
    for (const ShapeName& row : kShapeNames) {
        if (row.shape == shape) {
            return row.name;
        }
    }
    // Unreachable: the table is exhaustive by the static_assert above. The fallback keeps the
    // "never nullptr" contract for a value cast from an out-of-range integer.
    return "default";
}

} // namespace tessera::hal
