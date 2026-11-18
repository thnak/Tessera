// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

// `box-shadow` interpolation (CSS Values 4, "Interpolation of shadow lists"; issue #462).
//
// A shadow list animates layer by layer: offsets, blur and spread interpolate as lengths and the
// colour as a premultiplied colour. The shorter list is padded with transparent zero shadows that
// take the `inset` flag of the layer they pair with, so `none` fades a shadow in or out instead of
// snapping. A pair whose `inset` flags differ cannot interpolate, and neither can a value this
// parser does not understand; the caller keeps its discrete 50 % switch for those.
namespace tessera::ui::animation {

// The interpolated list at progress `t` as a `box-shadow` value, or nullopt when `from` and `to`
// are not both interpolable shadow lists. `t` outside [0, 1] extrapolates, like every other
// interpolated property.
[[nodiscard]] std::optional<std::string> interpolate_box_shadow(std::string_view from,
                                                                std::string_view to, double t);

} // namespace tessera::ui::animation
