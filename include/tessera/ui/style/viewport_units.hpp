// SPDX-License-Identifier: MIT
//
// The one viewport-unit table (issue #691). Before this file the engine carried three copies of it -
// `layout_style.cpp` for Yoga lengths, `grid_values.cpp` for track sizes, `media_query.cpp` for
// `@media` - and all three resolved only `vw` and `vh`, so `vmin`, `vmax` and the whole
// small/large/dynamic family silently failed to parse even though `stylesheet-v1.md` section 8.1
// lists them as V1.
//
// The small/large/dynamic variants exist because a mobile browser's viewport changes as its toolbars
// slide away: `sv*` is the smallest (toolbars shown), `lv*` the largest (toolbars hidden), `dv*` the
// current one. A desktop window has no such chrome, so all three are equal to the plain `v*` value.
// That is not a shortcut - it is the correct answer for a viewport whose chrome never moves - and
// `ViewportUnits::has_dynamic_chrome` records the assumption so a platform that can differ has one
// place to say so.

#pragma once

#include <cstdint>
#include <string_view>

namespace tessera::ui::style {

// The viewport a length resolves against, in logical pixels. `font_size` is what `em` means for the
// element being styled (its own computed font size, issue #608); `rem` is always the 16px root.
struct ViewportUnits {
    double width = 1280.0;
    double height = 720.0;
    double font_size = 16.0;
    // True when the platform's viewport chrome can move (a mobile browser's toolbars). When false,
    // `sv*`/`lv*`/`dv*` all resolve to the plain `v*` value, which is what a desktop window means.
    bool has_dynamic_chrome = false;
    // The small and large viewport extents, used only when `has_dynamic_chrome` is true. A platform
    // that reports them sets these; the defaults make them equal to `width`/`height`.
    double small_width = 0.0;
    double small_height = 0.0;
    double large_width = 0.0;
    double large_height = 0.0;
};

// Which viewport a `v*` unit measures.
enum class ViewportAxis : std::uint8_t {
    Width,
    Height,
    // The smaller of the two: `vmin`.
    Min,
    // The larger of the two: `vmax`.
    Max,
};

// Which viewport size a unit names.
enum class ViewportSize : std::uint8_t {
    // `v*`: the current viewport, which is what a desktop window always is.
    Default,
    // `sv*`: the smallest, with the platform's chrome shown.
    Small,
    // `lv*`: the largest, with the chrome hidden.
    Large,
    // `dv*`: the current one, which tracks the chrome.
    Dynamic,
};

// A parsed viewport unit: which size and axis, and the number in front of it.
struct ViewportLength {
    ViewportSize size = ViewportSize::Default;
    ViewportAxis axis = ViewportAxis::Width;
    double value = 0.0;
};

// Parses a unit suffix (`vw`, `svh`, `dvmin`, ...) into its size and axis. Returns false for
// anything else, so a caller can fall through to its own unit table.
[[nodiscard]] bool parse_viewport_unit(std::string_view unit, ViewportSize& size,
                                       ViewportAxis& axis) noexcept;

// Resolves a parsed viewport length to logical pixels.
[[nodiscard]] double resolve_viewport_length(const ViewportLength& length,
                                             const ViewportUnits& viewport) noexcept;

// The viewport extent a size/axis pair measures, before the percentage is applied.
[[nodiscard]] double viewport_extent(ViewportSize size, ViewportAxis axis,
                                     const ViewportUnits& viewport) noexcept;

} // namespace tessera::ui::style
