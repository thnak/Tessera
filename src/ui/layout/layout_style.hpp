// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <string>
#include <string_view>

#include <yoga/Yoga.h>

namespace tessera::ui::layout::detail {

// Lower-cases ASCII and trims whitespace of CSS keyword/number tokens.
[[nodiscard]] std::string lower(std::string_view value);
[[nodiscard]] std::string trim(std::string_view value);

// True when `value` is non-null and equals `keyword` after trim + lower-casing.
[[nodiscard]] bool is_keyword(const std::string* value, std::string_view keyword);

// Parses a finite float token; false (value untouched) on anything else.
[[nodiscard]] bool parse_number(std::string_view text, float& value);

// Maps the computed CSS declarations that Yoga understands onto `node` (display, flex, sizing,
// margin/padding/border, position, alignment, gap, ...). Unknown or invalid values are ignored.
void apply_style(YGNodeRef node, const style::ComputedStyle& style, const LayoutViewport& viewport);

} // namespace tessera::ui::layout::detail
