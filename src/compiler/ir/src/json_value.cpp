// SPDX-License-Identifier: MIT

#include "tessera/compiler/ir/json_value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace tessera::compiler::ir {

JsonValue JsonValue::boolean(bool value) {
    JsonValue json;
    json.data_ = value;
    return json;
}

JsonValue JsonValue::integer(std::int64_t value) {
    JsonValue json;
    json.data_ = value;
    return json;
}

JsonValue JsonValue::number(double value) {
    JsonValue json;
    if (std::isfinite(value)) {
        json.data_ = value;
    }
    return json;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue json;
    json.data_ = std::move(value);
    return json;
}

JsonValue JsonValue::array() {
    JsonValue json;
    json.data_ = Array{};
    return json;
}

JsonValue JsonValue::object() {
    JsonValue json;
    json.data_ = Object{};
    return json;
}

void JsonValue::push(JsonValue value) {
    if (auto* items = std::get_if<Array>(&data_)) {
        items->push_back(std::move(value));
    }
}

void JsonValue::set(std::string key, JsonValue value) {
    auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        return;
    }
    for (auto& member : *members) {
        if (member.key == key) {
            member.value = std::move(value);
            return;
        }
    }
    members->push_back(JsonMember{std::move(key), std::move(value)});
}

const JsonValue::Array* JsonValue::as_array() const {
    return std::get_if<Array>(&data_);
}

const JsonValue::Object* JsonValue::as_object() const {
    return std::get_if<Object>(&data_);
}

namespace {

void append_string(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            } else {
                out += raw;
            }
        }
    }
    out += '"';
}

void append_double(std::string& out, double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    out += text;
    if (text.find_first_of(".eE") == std::string_view::npos) {
        out += ".0";
    }
}

void indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void write_value(std::string& out, const JsonValue& value, int depth) {
    const auto& data = value.data();
    if (std::holds_alternative<std::monostate>(data)) {
        out += "null";
    } else if (const auto* flag = std::get_if<bool>(&data)) {
        out += *flag ? "true" : "false";
    } else if (const auto* integer = std::get_if<std::int64_t>(&data)) {
        out += std::to_string(*integer);
    } else if (const auto* number = std::get_if<double>(&data)) {
        append_double(out, *number);
    } else if (const auto* text = std::get_if<std::string>(&data)) {
        append_string(out, *text);
    } else if (const auto* items = value.as_array()) {
        if (items->empty()) {
            out += "[]";
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < items->size(); ++i) {
            indent(out, depth + 1);
            write_value(out, (*items)[i], depth + 1);
            out += i + 1 < items->size() ? ",\n" : "\n";
        }
        indent(out, depth);
        out += ']';
    } else if (const auto* members = value.as_object()) {
        if (members->empty()) {
            out += "{}";
            return;
        }
        std::vector<const JsonMember*> sorted;
        sorted.reserve(members->size());
        for (const auto& member : *members) {
            sorted.push_back(&member);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const JsonMember* a, const JsonMember* b) { return a->key < b->key; });
        out += "{\n";
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            indent(out, depth + 1);
            append_string(out, sorted[i]->key);
            out += ": ";
            write_value(out, sorted[i]->value, depth + 1);
            out += i + 1 < sorted.size() ? ",\n" : "\n";
        }
        indent(out, depth);
        out += '}';
    }
}

} // namespace

std::string to_text(const JsonValue& value) {
    std::string out;
    write_value(out, value, 0);
    out += '\n';
    return out;
}

} // namespace tessera::compiler::ir
