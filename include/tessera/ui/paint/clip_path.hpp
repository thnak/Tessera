// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::ui::paint::detail {

// `clip-path` basic shapes (ADR-068 slice 4, issue #672).
//
// The supported grammar is the CSS `clip-path` basic-shape subset:
//
//   none
//   inset( <length-percentage>{1,4} [ round <length-percentage>{1,4} ]? )
//   circle( <length-percentage>? [ at <position> ]? )
//   ellipse( <length-percentage>{2} [ at <position> ]? )
//
// `polygon()` is slice 5 and is rejected here with a documented "unsupported" result rather than
// silently ignored, so a caller can tell "no clip" from "a clip this build cannot draw".
//
// Percentages resolve against the reference box: horizontal against its width, vertical against its
// height. That is the CSS rule for `inset()` and for a `circle()`/`ellipse()` radius, and it is why
// the parser needs the box rather than only the string.

// One parsed basic shape, in the reference box's coordinate space.
struct ClipShape {
    enum class Kind : std::uint8_t { None, Inset, Circle, Ellipse };

    Kind kind = Kind::None;
    // `inset()`: the four edge insets (top, right, bottom, left) in pixels.
    float inset_top = 0.0f;
    float inset_right = 0.0f;
    float inset_bottom = 0.0f;
    float inset_left = 0.0f;
    // `inset(â€¦ round â€¦)`: the four corner radii (TL, TR, BR, BL) in pixels. A zero entry is a
    // square corner; the elliptical pair is not expressible in the CSS `round` shorthand this slice
    // accepts, so both components of a corner are equal.
    float radius_tl = 0.0f;
    float radius_tr = 0.0f;
    float radius_br = 0.0f;
    float radius_bl = 0.0f;
    // `circle()`/`ellipse()`: the centre and radii in pixels, relative to the reference box.
    float center_x = 0.0f;
    float center_y = 0.0f;
    float radius_x = 0.0f;
    float radius_y = 0.0f;

    [[nodiscard]] bool is_none() const {
        return kind == Kind::None;
    }
};

// The result of parsing a `clip-path` value. `unsupported` is distinct from `none`: a caller that
// sees it should paint unclipped and count the gap, not pretend the element asked for no clip.
struct ClipPathParse {
    ClipShape shape;
    bool unsupported = false;
    // Set when the value was malformed (a bad length, a missing argument). The caller treats it as
    // `none`, which is the CSS "invalid declaration is dropped" rule.
    bool invalid = false;
};

// Parses a `clip-path` value against `box`, the reference box in target pixels. `value` is the
// computed string, so a `var()` or a `calc()` has already been resolved by the cascade.
[[nodiscard]] ClipPathParse parse_clip_path(std::string_view value,
                                            const rendering::rhi::Rect& box);

// Converts a parsed shape into the clip record the renderer and the CPU reference both read. The
// record's `kind` is `Ellipse` for a circle/ellipse and `RoundedRect` for an inset, which is what
// the shader dispatches on.
[[nodiscard]] rendering::gpu::clip::ClipRecord to_clip_record(const ClipShape& shape,
                                                              const rendering::rhi::Rect& box);

} // namespace tessera::ui::paint::detail
