// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/forms/form_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tessera::ui::forms::detail {

// Counts UTF-8 code points (length rules use characters, not bytes; invalid bytes count as one).
std::size_t utf8_code_points(std::string_view text);

// Documented pattern language: '*' matches any run (including empty), '?' matches exactly one code
// point, every other code point is literal. Iterative and bounded (no regex, no allocation).
bool glob_match(std::string_view pattern, std::string_view text);

// Parses an optional sign followed by ASCII digits into int64; nullopt on
// empty/non-numeric/overflow.
std::optional<std::int64_t> parse_integer(std::string_view text);

// Evaluates one synchronous rule against an immutable value. `equal_to_value` is the target field's
// current value for EqualTo rules (nullptr when the target does not exist). Returns nullopt when
// the rule passes, otherwise the stable code/message for the issue.
std::optional<RuleIssueTemplate> evaluate_sync_rule(const SyncRule& rule, const FormValue& value,
                                                    const FormValue* equal_to_value);

} // namespace tessera::ui::forms::detail
