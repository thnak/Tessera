// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Token helpers shared by the linear and radial halves of the gradient grammar
// (`gradient.cpp`, `gradient_radial.cpp`). Internal to the style library; not installed.
namespace tessera::ui::style::detail {

// These forward to the shared implementations (issue #733) rather than owning a private copy: the
// grammar is one dialect, and a fix to `trim` or `lower` must reach every parser at once.
using tessera::detail::lower;
using tessera::detail::trim;

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

// A `<length>` token in px, or a bare `0`. The absolute units (`px`, `cm`, `mm`, `q`, `in`, `pt`,
// `pc`) convert at 96 dpi; false for any other unit (the grammar has no font or box to resolve
// `em`/`%` against).
std::optional<float> parse_px_length(std::string_view text);

// True when `text` is the start of a colour stop rather than a prelude token: its first
// space-separated token is a colour (so `red 20%` and `#fff 0 10px` count, `circle` and `from
// 30deg` do not).
bool looks_like_stop(std::string_view text);

// What a stop's position tokens mean. A linear or radial stop takes a percentage or a length (a
// fraction or px along the gradient line); a conic stop takes an angle or a percentage of the turn.
enum class StopDomain : std::uint8_t { Line, Turn };

// Parses `parts[first..]` (the comma-separated stops of a gradient body) into `stops`, the shape's
// `StopPositions` and `count`. Each part is `<color> <position>{0,2}`; two positions make two
// stops of one colour (CSS Images 4 section 3.5.2). The offsets come back fixed up when every
// position is a fraction; a px or missing position in a list with a px stop is left for
// `resolve_stop_positions`. False when a part is a colour hint or otherwise outside the grammar, or
// when the stops number fewer than 2 or more than `kMaxGradientStops`.
bool parse_stop_list(const std::vector<std::string_view>& parts, std::size_t first,
                     StopDomain domain, std::array<GradientStopValue, kMaxGradientStops>& stops,
                     StopPositions& positions, std::size_t& count);

// CSS Images 3 section 3.4.3 colour-stop fix-up over `count` authored positions: a missing
// first/last position is 0/100 %, a missing middle one is spread evenly between its neighbours, and
// a position behind an earlier one is raised to it. Resolves `positions` in place.
void fix_up_stop_offsets(std::array<std::optional<float>, kMaxGradientStops>& positions,
                         std::size_t count);

} // namespace tessera::ui::style::detail
