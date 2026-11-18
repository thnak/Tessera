// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// The `background-image: linear-gradient(...)` value (issue #457).
//
// One `linear-gradient()` with an angle (`deg`, `grad`, `rad`, `turn`) or a `to <side|corner>`
// direction and 2 to 8 colour stops, each with an optional percentage position. Anything else
// (radial/conic, `repeating-*`, several layers, `url()`, a length position, a colour hint) is not a
// value of this grammar and `parse_linear_gradient` returns nullopt, so the caller can report it
// instead of painting a guess.
namespace tessera::ui::style {

inline constexpr std::size_t kMaxGradientStops = 8;

struct GradientStopValue {
    float offset = 0.0f; // 0..1 along the gradient line, already fixed up
    ColorRgba color;
};

struct LinearGradientValue {
    // CSS convention: 0 points up, 90 right, 180 down (the default).
    float angle_degrees = 180.0f;
    std::array<GradientStopValue, kMaxGradientStops> stops{};
    std::size_t stop_count = 0;
};

// Offsets follow CSS Images 3 section 3.4.3: a missing first/last position is 0/100 %, a missing
// middle one is spread evenly between its neighbours, and a position behind an earlier one is
// raised to it. A corner direction (`to top right`) resolves to the diagonal angle of a square box;
// CSS bends it with the box's aspect ratio, which this does not.
[[nodiscard]] std::optional<LinearGradientValue> parse_linear_gradient(std::string_view text);

} // namespace tessera::ui::style
