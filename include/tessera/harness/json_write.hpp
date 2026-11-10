// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_value.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::harness {

enum class JsonWriteErrorCode {
    NonFiniteNumber, // JSON has no NaN/Infinity: report instead of writing invalid output
    InvalidUtf8,     // a string is not well-formed UTF-8, so the line would not be valid JSON
};

struct JsonWriteError {
    JsonWriteErrorCode code = JsonWriteErrorCode::NonFiniteNumber;
    // Byte offset, inside the document being built, where the offending string started. It points
    // at the opening quote of the value and is 0 for a number.
    std::size_t offset = 0;

    friend constexpr bool operator==(const JsonWriteError&, const JsonWriteError&) = default;
};

// Canonical serialization: no insignificant whitespace, object members in stored order, integers
// exact, doubles in the shortest round-trip form, control characters escaped with the two-character
// escapes where JSON defines one and `\u00XX` otherwise, and non-ASCII UTF-8 passed through
// unchanged. The same value therefore always produces the same bytes.
[[nodiscard]] std::expected<std::string, JsonWriteError> to_json(const JsonValue& value);

// Same encoding, appended to `out`. `out` is left unchanged when the value is rejected.
[[nodiscard]] std::expected<void, JsonWriteError> append_json(std::string& out,
                                                              const JsonValue& value);

} // namespace tessera::harness
