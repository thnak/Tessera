// SPDX-License-Identifier: MIT

// Shared field readers for the shader manifest: mandatory-member lookup plus string/integer
// extraction with the section 9 bounds. Keeping them out of the entry parser makes the shape
// rules of each field the only thing that file has to explain.

#include "shader_manifest_internal.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::schema::internal {

std::expected<const JsonValue*, ValidationError>
required_member(const JsonValue& object, std::string_view key, std::string_view object_path) {
    const JsonValue* value = find_member(object, key);
    if (value == nullptr) {
        return fail(ValidationErrorCode::MissingKey, member_path(object_path, key),
                    "required key is missing");
    }
    return value;
}

std::expected<std::string, ValidationError>
string_value(const JsonValue& value, std::string_view path, std::size_t max_length) {
    if (!value.is_string()) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected a string");
    }
    if (value.as_string().empty()) {
        return fail(ValidationErrorCode::InvalidValue, std::string(path),
                    "string must not be empty");
    }
    if (value.as_string().size() > max_length) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "string is too long");
    }
    return std::string(value.as_string());
}

std::expected<std::string, ValidationError> required_string(const JsonValue& object,
                                                            std::string_view key,
                                                            std::string_view object_path,
                                                            std::size_t max_length) {
    auto value = required_member(object, key, object_path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return string_value(**value, member_path(object_path, key), max_length);
}

std::expected<std::uint32_t, ValidationError>
integer_value(const JsonValue& value, std::string_view path, std::uint32_t max_value) {
    const auto integer = value.as_integer();
    if (!integer) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an integer");
    }
    if (*integer < 0) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "must not be negative");
    }
    if (static_cast<std::uint64_t>(*integer) > max_value) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "value is above the limit");
    }
    return static_cast<std::uint32_t>(*integer);
}

std::expected<std::uint32_t, ValidationError> required_integer(const JsonValue& object,
                                                               std::string_view key,
                                                               std::string_view object_path,
                                                               std::uint32_t max_value) {
    auto value = required_member(object, key, object_path);
    if (!value) {
        return std::unexpected(value.error());
    }
    return integer_value(**value, member_path(object_path, key), max_value);
}

} // namespace tessera::schema::internal
