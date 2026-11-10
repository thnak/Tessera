// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <unordered_map>

namespace tessera::ui::style::detail {

// CSS named colors (CSS Color 4) as 0xRRGGBB, keyed by lower-case name.
[[nodiscard]] const std::unordered_map<std::string, unsigned>& named_colors();

} // namespace tessera::ui::style::detail
