// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/gradient.hpp"

#include <string_view>

// The two shape halves of the gradient grammar. `gradient.cpp` owns the function-name dispatch and
// the repeating expansion; each half parses only the text between its own parentheses, so the two
// grammars cannot drift in how they read a stop list. Internal to the style library; not installed.
namespace tessera::ui::style::detail {

// Parses the inside of `linear-gradient(...)`. False when the value is outside the grammar.
bool parse_linear_body(std::string_view inner, LinearGradientValue& out);

// Parses the inside of `radial-gradient(...)`. False when the value is outside the grammar.
bool parse_radial_body(std::string_view inner, RadialGradientValue& out);

} // namespace tessera::ui::style::detail
