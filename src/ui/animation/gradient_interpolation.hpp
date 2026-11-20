// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

// `background-image` gradient interpolation (CSS Images 3 section 3.6; issues #457 and #613).
//
// Two gradients of the same shape interpolate stop by stop: the stop lists are padded to the longer
// one (a missing stop repeats its neighbour's colour at the same offset), each offset and colour
// blends, and the geometry blends too - the angle for a linear gradient, the centre and the two
// radii for a radial one. A shape change, a `repeating-` mismatch, or a value this parser does not
// understand cannot interpolate; the caller keeps its discrete 50 % switch for those.
namespace tessera::ui::animation {

// The interpolated `background-image` value at progress `t`, or nullopt when `from` and `to` are
// not both interpolable gradients. `t` outside [0, 1] extrapolates, like every other interpolated
// property.
[[nodiscard]] std::optional<std::string> interpolate_gradient(std::string_view from,
                                                              std::string_view to, double t);

} // namespace tessera::ui::animation
