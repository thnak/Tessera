// SPDX-License-Identifier: MIT

// The JSON value model used by the shader-manifest reader. The rationale for a schema-local
// decoder is in json.hpp; this file only holds construction and accessors.

#include "json.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace tessera::schema::internal {

JsonValue JsonValue::make_bool(bool value) {
    JsonValue json;
    json.type_ = JsonType::Bool;
    json.boolean_ = value;
    return json;
}

JsonValue JsonValue::make_integer(std::int64_t value) {
    JsonValue json;
    json.type_ = JsonType::Number;
    json.exact_integer_ = true;
    json.integer_ = value;
    json.number_ = static_cast<double>(value);
    return json;
}

JsonValue JsonValue::make_number(double value) {
    JsonValue json;
    json.type_ = JsonType::Number;
    json.number_ = value;
    return json;
}

JsonValue JsonValue::make_string(std::string value) {
    JsonValue json;
    json.type_ = JsonType::String;
    json.string_ = std::move(value);
    return json;
}

JsonValue JsonValue::make_array(Array values) {
    JsonValue json;
    json.type_ = JsonType::Array;
    json.array_ = std::move(values);
    return json;
}

JsonValue JsonValue::make_object(Object members) {
    JsonValue json;
    json.type_ = JsonType::Object;
    json.object_ = std::move(members);
    return json;
}

std::optional<std::int64_t> JsonValue::as_integer() const noexcept {
    if (type_ != JsonType::Number || !exact_integer_) {
        return std::nullopt;
    }
    return integer_;
}

const JsonValue::Array* JsonValue::as_array() const noexcept {
    return type_ == JsonType::Array ? &array_ : nullptr;
}

const JsonValue::Object* JsonValue::as_object() const noexcept {
    return type_ == JsonType::Object ? &object_ : nullptr;
}

} // namespace tessera::schema::internal
