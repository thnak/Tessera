// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

// `clip-path` basic-shape interpolation (CSS Shapes 1, "Interpolating basic shapes"; ADR-068
// slice 6, issue #674).
//
// Two shapes of the same function interpolate component by component: `inset()` (its four edges and
// four corner radii), `circle()` and `ellipse()` (radii and centre) and `polygon()` (every vertex,
// when both have the same vertex count and the same fill rule). Everything else cannot: `none`
// against a shape, two different functions, polygons of different size or fill rule, a pair of
// lengths in different units that is not zero, `url()`, `path()` and any value this reader does not
// understand. The caller keeps its discrete 50 % switch for those, which is what CSS specifies.
//
// This mirrors the grammar of `ui/paint/clip_path.cpp` but works on the unresolved text, because
// the animation layer has no reference box: a percentage stays a percentage, so the paint-time
// parse resolves both ends against the box the node actually has.
namespace tessera::ui::animation {

// The interpolated shape at progress `t` as a `clip-path` value, or nullopt when `from` and `to`
// are not both interpolable shapes of the same kind. `t` outside [0, 1] extrapolates like every
// other interpolated property, except that a radius is clamped at zero (a negative radius is not a
// shape).
[[nodiscard]] std::optional<std::string> interpolate_clip_path(std::string_view from,
                                                               std::string_view to, double t);

} // namespace tessera::ui::animation
