// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <optional>
#include <string_view>

// The colour functions that are not a fixed channel list: `light-dark()` (CSS Color 5 section 3)
// and `color-mix()` (CSS Color 5 section 4). Internal to the style library. Not installed.
//
// They live beside `color.cpp` rather than inside it because both need the OKLab transform
// (`color_oklch.hpp`) and a recursive call back into `parse_color`, and `color.cpp` is already at
// the size where a second responsibility would push it past the file limit.
namespace tessera::ui::style::detail {

// True when `text` is exactly one `light-dark(...)` or `color-mix(...)` call (case-insensitive
// function name, balanced parentheses, nothing after the closing one).
[[nodiscard]] bool is_color_function(std::string_view text);

// Evaluates one colour function. Returns nullopt when the text is not one of the two, or when it is
// malformed (wrong arity, an unknown interpolation space, an unparseable operand). A malformed
// function is an invalid colour, exactly like a malformed `rgb()`.
[[nodiscard]] std::optional<ColorRgba> evaluate_color_function(std::string_view text,
                                                               const ColorContext& context);

// Replaces every `light-dark()` and `color-mix()` call inside `value` with the colour it evaluates
// to, so a function nested in a larger value (`box-shadow: 0 0 4px light-dark(#000, #fff)`, a
// gradient stop, a border colour) resolves at computed-value time like `var()` does. The outermost
// call is evaluated first, which covers nesting because `evaluate_color_function` recurses.
//
// Returns false when a call is malformed. The caller then treats the declaration as invalid at
// computed-value time, the same rule an unresolvable `var()` follows (stylesheet-v1 section 15).
[[nodiscard]] bool substitute_color_functions(std::string& value, const ColorContext& context);

} // namespace tessera::ui::style::detail
