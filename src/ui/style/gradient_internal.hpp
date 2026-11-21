// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/gradient.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Token helpers shared by the linear and radial halves of the gradient grammar
// (`gradient.cpp`, `gradient_radial.cpp`). Internal to the style library; not installed.
namespace tessera::ui::style::detail {

std::string_view trim(std::string_view text);
std::string lower(std::string_view text);

// Splits at top-level commas (a colour such as rgba(0, 0, 0, .5) has commas of its own).
std::vector<std::string_view> split_commas(std::string_view text);

// Splits at top-level whitespace, so `rgba(0, 0, 0, .2)` and `10px 20px` split into one and two
// components respectively.
std::vector<std::string_view> split_spaces(std::string_view text);

// `strtof` plus the trailing unit text. False when there is no leading number or it is not finite.
bool parse_number(const std::string& text, float& value, std::string& unit);

// An `<angle>` token in degrees: `deg`, `grad`, `rad`, `turn`, or a bare `0`. False for any
// other unit. Shared by the linear and conic halves, which both measure from 12 o'clock.
std::optional<float> parse_angle(std::string_view text);

// A `<percentage>` token as a 0..1 fraction. False for any other unit.
std::optional<float> parse_percentage(std::string_view text);

// A `<length>` token in px, or a bare `0`. False for any other unit (the grammar has no box to
// resolve `em`/`%` against).
std::optional<float> parse_px_length(std::string_view text);

// `<color> <percentage>?`; the colour may itself contain spaces inside its parentheses.
bool parse_stop(std::string_view text, ColorRgba& color, std::optional<float>& position);

// CSS Images 3 section 3.4.3 colour-stop fix-up over `count` authored positions: a missing
// first/last position is 0/100 %, a missing middle one is spread evenly between its neighbours, and
// a position behind an earlier one is raised to it. Resolves `positions` in place.
void fix_up_stop_offsets(std::array<std::optional<float>, kMaxGradientStops>& positions,
                         std::size_t count);

} // namespace tessera::ui::style::detail
