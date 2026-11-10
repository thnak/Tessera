// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::harness {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

// Minimal JSON value for the line protocol. There is deliberately no third-party dependency: the
// codec needs a total parser, canonical output, and bounded input, not a general-purpose library.
//
// Object members keep insertion order. That is the whole determinism story on the write side:
// output bytes depend only on the order the caller (or the decoder) built the members in, never on
// a hash table's iteration order. Decoding therefore preserves wire order, and re-encoding a
// decoded document is byte-identical for canonical input.
//
// Numbers keep an exact int64 when the literal was an integer that fits, because protocol
// counters must not round through a double. Fractional/exponent literals are doubles. Integers
// outside int64 are rejected by the parser: ids above 2^53 travel as decimal strings
// (docs/specs/automation-ids-v1.md section 1).
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>;

    JsonValue() noexcept = default; // null

    [[nodiscard]] static JsonValue boolean(bool value);
    [[nodiscard]] static JsonValue integer(std::int64_t value);
    [[nodiscard]] static JsonValue number(double value);
    [[nodiscard]] static JsonValue string(std::string value);
    [[nodiscard]] static JsonValue array(Array values = {});
    [[nodiscard]] static JsonValue object(Object members = {});

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

    [[nodiscard]] std::optional<bool> as_bool() const noexcept;
    // Exact integer value; an integral double from a fractional literal does not qualify.
    [[nodiscard]] std::optional<std::int64_t> as_integer() const noexcept;
    // Any number as a double (exact integers included).
    [[nodiscard]] std::optional<double> as_number() const noexcept;
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;

    [[nodiscard]] const Array* as_array() const noexcept;
    [[nodiscard]] Array* as_array() noexcept;
    [[nodiscard]] const Object* as_object() const noexcept;
    [[nodiscard]] Object* as_object() noexcept;

    // Object lookup; nullptr when this is not an object or the key is absent.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    // Inserts `key`, or replaces the existing member in place to keep its position. A null value
    // becomes an empty object first; any other non-object type is left untouched and returns
    // false.
    bool set(std::string_view key, JsonValue value);
    [[nodiscard]] bool erase(std::string_view key);

    // Array append; a non-array value is left untouched and returns false.
    bool push_back(JsonValue value);

    // Members of an object or elements of an array; 0 for every other type.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // Structural equality on the canonical form: numbers compare by representation, so an exact
    // integer literal is not equal to a double literal with the same numeric value, and object
    // member order matters (it is part of the bytes this type exists to preserve).
    friend bool operator==(const JsonValue&, const JsonValue&) = default;

private:
    JsonType type_ = JsonType::Null;
    bool boolean_ = false;
    bool exact_integer_ = false;
    double number_ = 0.0;
    std::int64_t integer_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

} // namespace tessera::harness
