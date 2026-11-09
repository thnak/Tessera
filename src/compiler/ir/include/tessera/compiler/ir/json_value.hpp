// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tessera::compiler::ir {

struct JsonMember;

// A small JSON document model. Objects keep insertion order in memory; `to_text` always writes
// their keys in byte order, so output never depends on construction order.
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::vector<JsonMember>;

    JsonValue() = default; // null
    static JsonValue boolean(bool value);
    static JsonValue integer(std::int64_t value);
    static JsonValue number(double value); // finite doubles; others are written as null
    static JsonValue string(std::string value);
    static JsonValue array();
    static JsonValue object();

    // Appends to an array. No effect on other kinds.
    void push(JsonValue value);
    // Sets a member of an object, replacing an existing member of the same key.
    void set(std::string key, JsonValue value);

    [[nodiscard]] const Array* as_array() const;
    [[nodiscard]] const Object* as_object() const;
    [[nodiscard]] const std::variant<std::monostate, bool, std::int64_t, double, std::string, Array,
                                     Object>&
    data() const {
        return data_;
    }

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> data_;
};

struct JsonMember {
    std::string key;
    JsonValue value;
};

// Two-space indent, LF line ends, one trailing newline, keys in byte order, `[]` and `{}` for
// empty containers, integers in base 10, floats as the shortest text that parses back (always
// containing `.` or an exponent), control characters as \u00XX, no locale.
std::string to_text(const JsonValue& value);

} // namespace tessera::compiler::ir
