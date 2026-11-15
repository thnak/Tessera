// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/json_error.hpp"
#include "tessera/harness/json_value.hpp"

#include <cstddef>
#include <expected>
#include <string_view>

namespace tessera::harness {

// Bounds the recursion depth of the decoder (and the stack it consumes) for untrusted input.
// Protocol envelopes are two levels deep; 64 is generous for nested audit/trace payloads.
inline constexpr std::size_t kMaxJsonDepth = 64;

// The one JSON failure vocabulary, shared with the schema manifest reader and the runtime config
// reader (god-type audit section 4.4). The name stays a harness alias because
// `JsonParseError::code` is public surface this library's callers already use; the vocabulary
// behind it is shared, so the same failure cannot be spelled three ways.
using JsonParseErrorCode = tessera::detail::json::ErrorCode;

struct JsonParseError {
    JsonParseErrorCode code = JsonParseErrorCode::UnexpectedEnd;
    std::size_t offset = 0;

    friend constexpr bool operator==(const JsonParseError&, const JsonParseError&) = default;
};

// Strict RFC 8259 decoder, plus the rules the protocol needs on top of it:
// - UTF-8 is validated before decoding, so a string is always well-formed UTF-8;
// - duplicate object keys are rejected instead of last-wins (an ambiguous envelope is a bug);
// - raw control characters in strings are rejected, and `\u` escapes must be valid UTF-16
//   (surrogates must pair) and are encoded to UTF-8;
// - number literals must match the JSON grammar exactly, integers must fit int64, and a value
//   must be finite;
// - nesting is bounded by kMaxJsonDepth and nothing may follow the top-level value.
[[nodiscard]] std::expected<JsonValue, JsonParseError>
parse_json(std::string_view text, std::size_t max_depth = kMaxJsonDepth);

} // namespace tessera::harness
