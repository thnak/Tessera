// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal strict JSON document model and reader for the shader manifest
// (docs/specs/shader-manifest-v1.md section 9).
//
// Why a schema-local reader and not a shared one: `tessera_schema` is a leaf library by contract
// (docs/specs/jsx-intrinsics-v1/machine-readable.md section 4, enforced by the LINK_LIBRARIES guard
// in src/schema/CMakeLists.txt), so it may not link `tessera_ir` or `tessera_harness`. The IR's
// `JsonValue` exposes a writer only (no decoder at all), and the harness decoder lives behind a
// library the schema may not depend on. A decoder here therefore duplicates nothing silently; it
// is the smallest reader the manifest needs (RFC 8259, UTF-8 validation, duplicate-key rejection,
// bounded depth), and the public API carries no JSON types across the module seam.
namespace tessera::schema::internal {

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
    NumberOutOfRange,
    InvalidEscape,
    InvalidUtf16Escape,
    ControlCharacter,
    DuplicateKey,
    NestingTooDeep,
    InvalidUtf8,
};

struct JsonParseError {
    JsonErrorCode code = JsonErrorCode::UnexpectedEnd;
    std::size_t offset = 0;
};

// Object members keep document order. The manifest validator reports the first offending key in
// that order, so the error is deterministic for a given document.
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() noexcept = default; // null

    [[nodiscard]] static JsonValue make_bool(bool value);
    [[nodiscard]] static JsonValue make_integer(std::int64_t value);
    [[nodiscard]] static JsonValue make_number(double value);
    [[nodiscard]] static JsonValue make_string(std::string value);
    [[nodiscard]] static JsonValue make_array(Array values);
    [[nodiscard]] static JsonValue make_object(Object members);

    [[nodiscard]] JsonType type() const noexcept {
        return type_;
    }
    [[nodiscard]] bool is_null() const noexcept {
        return type_ == JsonType::Null;
    }
    [[nodiscard]] bool is_bool() const noexcept {
        return type_ == JsonType::Bool;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return type_ == JsonType::Number;
    }
    [[nodiscard]] bool is_string() const noexcept {
        return type_ == JsonType::String;
    }
    [[nodiscard]] bool is_array() const noexcept {
        return type_ == JsonType::Array;
    }
    [[nodiscard]] bool is_object() const noexcept {
        return type_ == JsonType::Object;
    }

    [[nodiscard]] bool as_bool() const noexcept {
        return boolean_;
    }
    // Exact integer when the literal had no fraction or exponent and fit int64.
    [[nodiscard]] std::optional<std::int64_t> as_integer() const noexcept;
    [[nodiscard]] double as_number() const noexcept {
        return number_;
    }
    [[nodiscard]] std::string_view as_string() const noexcept {
        return string_;
    }
    [[nodiscard]] const Array* as_array() const noexcept;
    [[nodiscard]] const Object* as_object() const noexcept;

private:
    JsonType type_ = JsonType::Null;
    bool boolean_ = false;
    bool exact_integer_ = false;
    std::int64_t integer_ = 0;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

// Strict RFC 8259 decoder with the rules the manifest needs: UTF-8 is validated, duplicate keys
// are rejected, raw control characters and malformed escapes are rejected, numbers must match the
// JSON grammar (integers must fit int64, all numbers must be finite), nesting is bounded by
// `max_depth`, and nothing may follow the top-level value.
[[nodiscard]] std::expected<JsonValue, JsonParseError> parse_json(std::string_view text,
                                                                  std::size_t max_depth);

// True when `text` is well-formed UTF-8 (no overlongs, surrogates, or code points above U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

} // namespace tessera::schema::internal
