// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal strict JSON reader for the `runtime` object of `tessera.json`.
//
// Why a reader lives here instead of the harness decoder: `tessera_runtime` is below the harness
// and the schema library in the link graph (the schema's own JSON reader documents the same
// constraint), so it may not depend on either. This is the smallest RFC 8259 decoder the loader
// needs - object/array/scalar values, `\u` escapes with surrogate pairing, duplicate-key rejection,
// UTF-8 validation and a bounded nesting depth - and no JSON type crosses the module seam.
namespace tessera::runtime::internal {

enum class JsonType : std::uint8_t {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

enum class JsonErrorCode : std::uint8_t {
    UnexpectedEnd,
    UnexpectedCharacter,
    TrailingContent,
    InvalidLiteral,
    InvalidNumber,
    InvalidEscape,
    DuplicateKey,
    NestingTooDeep,
    InvalidUtf8,
};

struct JsonParseError {
    JsonErrorCode code = JsonErrorCode::UnexpectedEnd;
    std::size_t offset = 0;
};

// A decoded value. `offset` is the byte offset of the first character of the value in the input, so
// a validation error can point at the member it rejected. Members keep document order: warnings and
// the first-error choice are therefore deterministic.
struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolean = false;
    bool exact_integer = false; // the literal had no fraction and no exponent
    bool non_finite = false;    // the literal overflowed double (e.g. 1e400); JSON has no NaN/inf
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::size_t offset = 0;

    [[nodiscard]] bool is_null() const noexcept {
        return type == JsonType::Null;
    }
    [[nodiscard]] bool is_bool() const noexcept {
        return type == JsonType::Bool;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return type == JsonType::Number;
    }
    [[nodiscard]] bool is_string() const noexcept {
        return type == JsonType::String;
    }
    [[nodiscard]] bool is_array() const noexcept {
        return type == JsonType::Array;
    }
    [[nodiscard]] bool is_object() const noexcept {
        return type == JsonType::Object;
    }
    // First member named `key`, or nullptr. Duplicate keys never reach the DOM.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
};

// Decodes one UTF-8 document. `max_depth` bounds object/array nesting (a scalar top-level value is
// depth 0). Nothing may follow the top-level value except JSON whitespace.
[[nodiscard]] std::expected<JsonValue, JsonParseError> parse_json(std::string_view text,
                                                                  std::size_t max_depth);

} // namespace tessera::runtime::internal
