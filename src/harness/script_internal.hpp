// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/script.hpp"

#include <cstddef>
#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::harness::detail {

// Helpers shared by the script document parser (`script.cpp`) and its profile/app/step parser
// (`script_step.cpp`). Not public API: both halves apply the same closed-set and bounded-field
// rules, and those rules live in one place instead of drifting apart.

[[nodiscard]] ScriptError script_error(ScriptErrorCode code, std::string message,
                                       std::string detail = {}, std::size_t step = kNoScriptStep);

// Rejects a member the document does not define. A misspelled `uiId` must fail loudly rather than
// let a script run a different test while reporting success.
[[nodiscard]] std::optional<ScriptError>
reject_unknown(const JsonValue& value, std::initializer_list<std::string_view> allowed,
               std::size_t step, std::string_view what);

[[nodiscard]] std::optional<std::string> read_string(const JsonValue* value, std::size_t max_bytes);

[[nodiscard]] std::expected<ScriptProfile, ScriptError>
parse_script_profile(const JsonValue& value);
[[nodiscard]] std::expected<std::string, ScriptError> parse_script_app(const JsonValue& value);
[[nodiscard]] std::expected<ScriptStep, ScriptError> parse_script_step(const JsonValue& value,
                                                                       std::size_t index);

} // namespace tessera::harness::detail
