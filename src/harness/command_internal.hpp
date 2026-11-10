// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/command.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::harness::detail {

// Helpers shared by the command and command-result codecs. They are not public API: both codecs
// apply the same rules, and the rules live in one place instead of drifting apart.

[[nodiscard]] std::unexpected<ProtocolError> invalid_argument(std::string message,
                                                              std::string detail);

// Rejects a field the payload does not define. A typo (`include_hidden` for `includeHidden`) must
// fail loudly: silently ignoring it would run a different query than the agent asked for while
// reporting success.
[[nodiscard]] std::optional<ProtocolError>
reject_unknown(const JsonValue& value, std::initializer_list<std::string_view> allowed);

// UiIds are decimal strings on the wire. A small JSON number is accepted for hand-written clients:
// the parser keeps integers exact up to int64, so the only rule is "non-negative and exact".
[[nodiscard]] bool read_ui_id(const JsonValue& value, UiId& out);
[[nodiscard]] std::optional<std::string> read_string(const JsonValue* value);

} // namespace tessera::harness::detail
