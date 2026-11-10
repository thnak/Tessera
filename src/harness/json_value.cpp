// SPDX-License-Identifier: MIT

#include "tessera/harness/json_value.hpp"

#include <utility>

namespace tessera::harness {

JsonValue JsonValue::boolean(bool value) {
    JsonValue result;
    result.type_ = JsonType::Bool;
    result.boolean_ = value;
    return result;
}

JsonValue JsonValue::integer(std::int64_t value) {
    JsonValue result;
    result.type_ = JsonType::Number;
    result.exact_integer_ = true;
    result.integer_ = value;
    result.number_ = static_cast<double>(value);
    return result;
}

JsonValue JsonValue::number(double value) {
    JsonValue result;
    result.type_ = JsonType::Number;
    result.number_ = value;
    return result;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue result;
    result.type_ = JsonType::String;
    result.string_ = std::move(value);
    return result;
}

JsonValue JsonValue::array(Array values) {
    JsonValue result;
    result.type_ = JsonType::Array;
    result.array_ = std::move(values);
    return result;
}

JsonValue JsonValue::object(Object members) {
    JsonValue result;
    result.type_ = JsonType::Object;
    result.object_ = std::move(members);
    return result;
}

std::optional<bool> JsonValue::as_bool() const noexcept {
    if (type_ != JsonType::Bool) {
        return std::nullopt;
    }
    return boolean_;
}

std::optional<std::int64_t> JsonValue::as_integer() const noexcept {
    if (type_ != JsonType::Number || !exact_integer_) {
        return std::nullopt;
    }
    return integer_;
}

std::optional<double> JsonValue::as_number() const noexcept {
    if (type_ != JsonType::Number) {
        return std::nullopt;
    }
    return number_;
}

std::optional<std::string_view> JsonValue::as_string() const noexcept {
    if (type_ != JsonType::String) {
        return std::nullopt;
    }
    return std::string_view(string_);
}

const JsonValue::Array* JsonValue::as_array() const noexcept {
    return type_ == JsonType::Array ? &array_ : nullptr;
}

JsonValue::Array* JsonValue::as_array() noexcept {
    return type_ == JsonType::Array ? &array_ : nullptr;
}

const JsonValue::Object* JsonValue::as_object() const noexcept {
    return type_ == JsonType::Object ? &object_ : nullptr;
}

JsonValue::Object* JsonValue::as_object() noexcept {
    return type_ == JsonType::Object ? &object_ : nullptr;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != JsonType::Object) {
        return nullptr;
    }
    for (const Member& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

bool JsonValue::set(std::string_view key, JsonValue value) {
    if (type_ == JsonType::Null) {
        type_ = JsonType::Object;
    }
    if (type_ != JsonType::Object) {
        return false;
    }
    for (Member& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return true;
        }
    }
    object_.emplace_back(std::string(key), std::move(value));
    return true;
}

bool JsonValue::erase(std::string_view key) {
    if (type_ != JsonType::Object) {
        return false;
    }
    for (auto it = object_.begin(); it != object_.end(); ++it) {
        if (it->first == key) {
            object_.erase(it);
            return true;
        }
    }
    return false;
}

bool JsonValue::push_back(JsonValue value) {
    if (type_ != JsonType::Array) {
        return false;
    }
    array_.push_back(std::move(value));
    return true;
}

std::size_t JsonValue::size() const noexcept {
    if (type_ == JsonType::Array) {
        return array_.size();
    }
    if (type_ == JsonType::Object) {
        return object_.size();
    }
    return 0;
}

bool JsonValue::empty() const noexcept {
    return size() == 0;
}

} // namespace tessera::harness
