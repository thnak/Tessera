// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::ui::style::detail {

// The text scanning `var()` and `env()` substitution share (var_resolve.cpp, var_scan.cpp): CSS
// function names are matched ASCII-case-insensitively, strings are skipped whole, and a fallback is
// everything after the first comma at nesting depth zero.
constexpr int kMaxVarDepth = 32;

[[nodiscard]] char ascii_lower(char c);
[[nodiscard]] bool is_custom_property_name(std::string_view name);
[[nodiscard]] bool is_var_function(std::string_view value, std::size_t at);
[[nodiscard]] bool is_env_function(std::string_view value, std::size_t at);
// A `var()` or `env()` call outside strings.
[[nodiscard]] bool contains_var_function(std::string_view value);
[[nodiscard]] std::size_t skip_string(std::string_view value, std::size_t at);
[[nodiscard]] std::size_t match_paren(std::string_view value, std::size_t open);
[[nodiscard]] std::size_t find_comma(std::string_view text);
[[nodiscard]] std::string trim(std::string_view text);
// An `env()` name with its arguments, tokens separated by single spaces.
[[nodiscard]] std::string normalize_env_name(std::string_view text);

} // namespace tessera::ui::style::detail
