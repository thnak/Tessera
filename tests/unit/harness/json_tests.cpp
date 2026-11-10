// SPDX-License-Identifier: MIT

#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_value.hpp"
#include "tessera/harness/json_write.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using tessera::harness::JsonParseError;
using tessera::harness::JsonParseErrorCode;
using tessera::harness::JsonType;
using tessera::harness::JsonValue;
using tessera::harness::JsonWriteErrorCode;
using tessera::harness::parse_json;
using tessera::harness::to_json;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_json: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::optional<JsonParseErrorCode> parse_error(std::string_view text) {
    const auto parsed = parse_json(text);
    if (parsed.has_value()) {
        return std::nullopt;
    }
    return parsed.error().code;
}

bool rejects(std::string_view text, JsonParseErrorCode code, const char* message) {
    return expect(parse_error(text) == code, message);
}

std::string written(const JsonValue& value) {
    const auto json = to_json(value);
    return json.has_value() ? *json : std::string("<write failed>");
}

void check_rejections() {
    using enum JsonParseErrorCode;
    rejects("", UnexpectedEnd, "empty document");
    rejects("   ", UnexpectedEnd, "whitespace only");
    rejects("[1,]", UnexpectedCharacter, "trailing array comma");
    rejects("{\"a\":1,}", UnexpectedCharacter, "trailing object comma");
    rejects("01", TrailingContent, "leading zero");
    rejects("+1", UnexpectedCharacter, "leading plus");
    rejects(".5", UnexpectedCharacter, "leading dot");
    rejects("1.", InvalidNumber, "missing fraction digits");
    rejects("1e", InvalidNumber, "missing exponent digits");
    rejects("-", UnexpectedEnd, "lonely minus");
    rejects("NaN", UnexpectedCharacter, "NaN is not JSON");
    rejects("Infinity", UnexpectedCharacter, "Infinity is not JSON");
    rejects("'a'", UnexpectedCharacter, "single quotes");
    rejects("{a:1}", UnexpectedCharacter, "unquoted key");
    rejects("{\"a\" 1}", UnexpectedCharacter, "missing colon");
    rejects("\"a\x01\"", ControlCharacterInString, "raw control character");
    rejects("\"\\x\"", InvalidEscape, "unknown escape");
    rejects("\"\\u12\"", InvalidUtf16Escape, "truncated unicode escape");
    rejects("\"\\u12g4\"", InvalidUtf16Escape, "bad unicode digit");
    rejects("\"\\ud800\"", InvalidUtf16Escape, "lone high surrogate");
    rejects("\"\\udc00\"", InvalidUtf16Escape, "lone low surrogate");
    rejects("\"\\ud800\\u0041\"", InvalidUtf16Escape, "unpaired high surrogate");
    rejects("\"\\ud800x\"", InvalidUtf16Escape, "high surrogate without a pair");
    rejects("\xC0\x80", InvalidUtf8, "invalid raw utf-8");
    rejects("{\"a\":1,\"a\":2}", DuplicateKey, "duplicate key");
    rejects("1 2", TrailingContent, "trailing content");
    rejects("truex", InvalidLiteral, "literal prefix");
    rejects("9223372036854775808", NumberOutOfRange, "positive int64 overflow");
    rejects("-9223372036854775809", NumberOutOfRange, "negative int64 overflow");
    rejects("1e999", NumberOutOfRange, "double overflow");

    std::string deep;
    deep.append(65, '[');
    deep.append(65, ']');
    rejects(deep, NestingTooDeep, "default depth limit");

    // The depth limit is a parameter, not a fixed constant, so a caller can tighten it.
    const auto shallow = parse_json("[[1]]", 2);
    expect(shallow.has_value(), "depth limit is configurable (inclusive)");
    const auto too_deep = parse_json("[[[1]]]", 2);
    expect(!too_deep.has_value() && too_deep.error().code == NestingTooDeep, "custom depth limit");
}

void check_scalars() {
    const auto null_value = parse_json("null");
    expect(null_value.has_value() && null_value->is_null(), "null");
    expect(!null_value->as_bool().has_value(), "null is not bool");
    expect(written(*null_value) == "null", "null writes");

    const auto boolean = parse_json(" true ");
    expect(boolean.has_value() && boolean->as_bool() == true, "true with whitespace");
    expect(written(*boolean) == "true", "bool writes");

    const auto integer = parse_json("-42");
    expect(integer.has_value() && integer->type() == JsonType::Number, "integer type");
    expect(integer->as_integer() == std::int64_t{-42}, "integer exact");
    expect(integer->as_number() == std::optional<double>{-42.0}, "integer as double");
    expect(written(*integer) == "-42", "integer writes");

    const auto maximum = parse_json("9223372036854775807");
    expect(maximum->as_integer() == std::numeric_limits<std::int64_t>::max(), "int64 max");
    expect(written(*maximum) == "9223372036854775807", "int64 max writes exactly");
    const auto minimum = parse_json("-9223372036854775808");
    expect(minimum->as_integer() == std::numeric_limits<std::int64_t>::min(), "int64 min");

    const auto fraction = parse_json("1.5e2");
    expect(fraction->as_number() == std::optional<double>{150.0}, "exponent value");
    expect(!fraction->as_integer().has_value(), "fraction is not an exact integer");
    expect(written(*fraction) == "150", "shortest double form");

    const auto negative_zero = parse_json("-0.0");
    expect(negative_zero->as_number() == std::optional<double>{0.0}, "negative zero value");
}

void check_strings() {
    const auto escapes = parse_json("\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"");
    expect(escapes.has_value(), "escape forms parse");
    expect(escapes->as_string() == std::optional<std::string_view>{"a\"b\\c/d\be\ff\ng\rh\ti"},
           "escape forms decode");

    const auto unicode = parse_json("\"caf\\u00e9 \\u2713\"");
    expect(unicode.has_value(), "unicode escapes parse");
    expect(unicode->as_string() == std::optional<std::string_view>{"caf\xC3\xA9 \xE2\x9C\x93"},
           "unicode escapes become utf-8");
    expect(written(*unicode) == "\"caf\xC3\xA9 \xE2\x9C\x93\"", "utf-8 passes through");

    const auto pair = parse_json("\"\\uD83D\\uDE00\"");
    expect(pair->as_string() == std::optional<std::string_view>{"\xF0\x9F\x98\x80"},
           "surrogate pair becomes four-byte utf-8");
    expect(written(*pair) == "\"\xF0\x9F\x98\x80\"", "four-byte utf-8 passes through");

    const auto escaped_output = written(JsonValue::string("a\"b\\c\nd\te\x01"));
    expect(escaped_output == "\"a\\\"b\\\\c\\nd\\te\\u0001\"", "writer escaping");
}

void check_containers() {
    const std::string_view text = "{\"id\":\"18446744073709551615\",\"ok\":true,\"values\":[1,-2,1."
                                  "5],\"nested\":{\"k\":null}}";
    const auto document = parse_json(text);
    expect(document.has_value() && document->is_object(), "object document");
    expect(document->find("id")->as_string() ==
               std::optional<std::string_view>{"18446744073709551615"},
           "decimal-string id");
    expect(document->find("ok")->as_bool() == std::optional<bool>{true}, "bool member");
    expect(document->find("values")->size() == 3, "array size");
    expect(document->find("values")->as_array()->at(2).as_number() == std::optional<double>{1.5},
           "array element");
    expect(document->find("nested")->find("k")->is_null(), "nested null");
    expect(document->find("missing") == nullptr, "missing member");
    expect(document->find("id")->find("nested") == nullptr, "find on a non-object");

    // Canonical input round-trips byte for byte, including member order.
    expect(written(*document) == text, "canonical round-trip");
    // Re-serializing is stable: the bytes never depend on a second pass.
    expect(written(*document) == written(*document), "repeat write identical");

    const std::string reordered = "{\"z\":1,\"a\":2,\"m\":3}";
    const auto parsed = parse_json(reordered);
    expect(written(*parsed) == reordered, "member order is preserved, not sorted");

    const auto empty_object = parse_json("{}");
    expect(empty_object->empty(), "empty object");
    expect(written(*empty_object) == "{}", "empty object writes");
    const auto empty_array = parse_json("[]");
    expect(written(*empty_array) == "[]", "empty array writes");
    expect(parse_json("[ 1 , 2 ]")->size() == 2, "whitespace inside containers");
}

void check_value_builder() {
    JsonValue object = JsonValue::object();
    expect(object.set("a", JsonValue::integer(1)), "set appends");
    expect(object.set("b", JsonValue::integer(2)), "set appends again");
    expect(object.set("a", JsonValue::integer(3)), "set replaces");
    expect(written(object) == "{\"a\":3,\"b\":2}", "replace keeps position");
    expect(object.erase("a"), "erase existing");
    expect(!object.erase("a"), "erase missing");
    expect(written(object) == "{\"b\":2}", "erase writes");

    JsonValue promoted;
    expect(promoted.set("k", JsonValue::boolean(true)), "null promotes to object");
    expect(promoted.is_object(), "promoted type");

    JsonValue array = JsonValue::array();
    expect(array.push_back(JsonValue::integer(1)), "array push");
    expect(!object.push_back(JsonValue::integer(1)), "push on object fails");
    expect(array.as_integer() == std::nullopt, "accessor on wrong type");
    expect(!JsonValue::string("x").set("k", JsonValue()), "set on string fails");
}

void check_writer_rejections() {
    const JsonValue infinite = JsonValue::number(std::numeric_limits<double>::infinity());
    expect(to_json(infinite).error().code == JsonWriteErrorCode::NonFiniteNumber,
           "non-finite rejected");

    std::string bad;
    bad.push_back('\xC0');
    bad.push_back('\x80');
    const JsonValue invalid = JsonValue::string(bad);
    expect(to_json(invalid).error().code == JsonWriteErrorCode::InvalidUtf8,
           "invalid utf-8 rejected");

    std::string untouched = "prefix";
    const auto appended = tessera::harness::append_json(untouched, infinite);
    expect(!appended.has_value(), "append reports the rejection");
    expect(untouched == "prefix", "append leaves no partial output");
}

} // namespace

int main() {
    check_scalars();
    check_strings();
    check_containers();
    check_rejections();
    check_value_builder();
    check_writer_rejections();
    if (failures != 0) {
        std::cerr << "harness_json: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
