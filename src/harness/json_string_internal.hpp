// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_parse.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::harness::detail {

// Decodes the JSON string literal that starts at text[index], which must be the opening quote.
// Advances `index` past the closing quote and returns the decoded UTF-8 bytes. Escape handling
// lives here so the value parser stays a small recursive-descent skeleton; the rules are still the
// strict ones documented in json_parse.hpp.
[[nodiscard]] std::expected<std::string, JsonParseError> decode_json_string(std::string_view text,
                                                                            std::size_t& index);

} // namespace tessera::harness::detail
