// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

// Value grammar of the three interaction properties (stylesheet-v1 section 11, issue #247):
//   pointer-events: auto | none
//   user-select:    auto | none | text
//   aspect-ratio:   auto | <number> | <number> / <number> (both terms positive)
// One source for the declaration validator (property_registry.cpp), `@supports` (supports.cpp) and
// the computed-style -> scene sync (interaction_style.hpp). CSS-wide keywords and `var()` are the
// cascade's business and are never a value-form diagnostic.

[[nodiscard]] bool is_interaction_value_property(std::string_view property);

// Case-insensitive and trimmed. True for the three properties above when `value` is one of the
// implemented forms; true for every other property (the caller only asks about interaction ones).
[[nodiscard]] bool is_supported_interaction_value(std::string_view property,
                                                  std::string_view value);

// Appends exactly one `StyleDiagnosticKind::UnsupportedValue` when a recognised interaction
// property carries a form V1 does not implement: `all`/`contain` for `user-select`, the SVG-only
// `pointer-events` keywords (`visiblePainted`, `fill`, ...), or a non-ratio `aspect-ratio`. The
// declaration itself is kept by the caller, so the AST stays complete (section 11: unsupported
// value forms are reported, never silent).
void report_unsupported_interaction_value(std::string_view property, std::string_view value,
                                          std::size_t offset, std::vector<StyleDiagnostic>& out);

} // namespace tessera::ui::style
