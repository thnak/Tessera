// SPDX-License-Identifier: MIT

// The JSON text writer: key order, escapes, numbers, layout.

#include "ir_test_util.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using namespace ir_test;
using tessera::compiler::ir::JsonValue;
using tessera::compiler::ir::to_text;

std::string text_of_string(const std::string& value) {
    return to_text(JsonValue::string(value));
}

void test_keys_are_sorted_by_byte_order() {
    JsonValue forward = JsonValue::object();
    forward.set("b", JsonValue::integer(1));
    forward.set("a", JsonValue::integer(2));
    forward.set("B", JsonValue::integer(3));
    forward.set("aa", JsonValue::integer(4));
    JsonValue backward = JsonValue::object();
    backward.set("aa", JsonValue::integer(4));
    backward.set("B", JsonValue::integer(3));
    backward.set("a", JsonValue::integer(2));
    backward.set("b", JsonValue::integer(1));
    const std::string expected = "{\n  \"B\": 3,\n  \"a\": 2,\n  \"aa\": 4,\n  \"b\": 1\n}\n";
    check_eq(to_text(forward), expected, "uppercase sorts before lowercase, prefix before longer");
    check_eq(to_text(backward), expected, "construction order does not matter");
}

void test_set_replaces_a_member() {
    JsonValue object = JsonValue::object();
    object.set("k", JsonValue::integer(1));
    object.set("k", JsonValue::integer(2));
    check_eq(to_text(object), "{\n  \"k\": 2\n}\n", "one member per key, last value wins");
    JsonValue not_object = JsonValue::integer(1);
    not_object.set("k", JsonValue::integer(2));
    not_object.push(JsonValue::integer(2));
    check_eq(to_text(not_object), "1\n", "set and push ignore other kinds");
}

void test_string_escapes() {
    check_eq(text_of_string("a\"b"), "\"a\\\"b\"\n", "quote");
    check_eq(text_of_string("a\\b"), "\"a\\\\b\"\n", "backslash");
    check_eq(text_of_string("\b\f\n\r\t"), "\"\\b\\f\\n\\r\\t\"\n", "short escapes");
    check_eq(text_of_string(std::string("\x01\x1f", 2)), "\"\\u0001\\u001f\"\n",
             "other control characters");
    check_eq(text_of_string(std::string("a\0b", 3)), "\"a\\u0000b\"\n", "NUL");
    check_eq(text_of_string("\x7f"), "\"\x7f\"\n", "DEL is not escaped");
    check_eq(text_of_string("\xC3\xA9 /"), "\"\xC3\xA9 /\"\n", "UTF-8 and slash pass through");
    JsonValue key = JsonValue::object();
    key.set("a\"b", JsonValue::boolean(true));
    check_eq(to_text(key), "{\n  \"a\\\"b\": true\n}\n", "keys are escaped too");
}

void test_numbers() {
    check_eq(to_text(JsonValue::integer(0)), "0\n", "zero");
    check_eq(to_text(JsonValue::integer(-42)), "-42\n", "negative integer");
    check_eq(to_text(JsonValue::integer(INT64_MIN)), "-9223372036854775808\n", "int64 minimum");
    check_eq(to_text(JsonValue::number(1.0)), "1.0\n", "float with integer value keeps .0");
    check_eq(to_text(JsonValue::number(0.1)), "0.1\n", "shortest round-trip text");
    check_eq(to_text(JsonValue::number(-0.0)), "-0.0\n", "negative zero");
    check_eq(to_text(JsonValue::number(1e20)), "1e+20\n", "exponent form needs no .0");
    check_eq(to_text(JsonValue::number(123456.789)), "123456.789\n", "ordinary float");
    check_eq(to_text(JsonValue::number(std::numeric_limits<double>::quiet_NaN())), "null\n",
             "NaN has no JSON form");
    check_eq(to_text(JsonValue::number(std::numeric_limits<double>::infinity())), "null\n",
             "infinity has no JSON form");
}

void test_layout() {
    check_eq(to_text(JsonValue{}), "null\n", "null");
    check_eq(to_text(JsonValue::boolean(false)), "false\n", "false");
    check_eq(to_text(JsonValue::array()), "[]\n", "empty array");
    check_eq(to_text(JsonValue::object()), "{}\n", "empty object");

    JsonValue inner = JsonValue::array();
    inner.push(JsonValue::integer(1));
    inner.push(JsonValue::object());
    JsonValue nested = JsonValue::object();
    nested.set("z", std::move(inner));
    nested.set("a", JsonValue::string("x"));
    check_eq(to_text(nested), "{\n  \"a\": \"x\",\n  \"z\": [\n    1,\n    {}\n  ]\n}\n",
             "two-space indent, one item per line, LF, one trailing newline");
}

} // namespace

int main() {
    test_keys_are_sorted_by_byte_order();
    test_set_replaces_a_member();
    test_string_escapes();
    test_numbers();
    test_layout();
    return finish("ir json value tests");
}
