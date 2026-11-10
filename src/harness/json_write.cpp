// SPDX-License-Identifier: MIT

#include "tessera/harness/json_write.hpp"
#include "tessera/harness/utf8.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <utility>

namespace tessera::harness {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

// Escapes one string. `offset` is the position where the value starts, so a rejection can point at
// it. Non-ASCII bytes pass through because the value type only ever holds byte sequences that the
// decoder validated or the callers built from validated text; that invariant is re-checked here
// because a bad byte would corrupt every later frame on the stream, not just this one.
std::expected<void, JsonWriteError> append_string(std::string& out, std::string_view text,
                                                  std::size_t offset) {
    std::size_t bad = 0;
    if (!is_valid_utf8(text, &bad)) {
        return std::unexpected(JsonWriteError{JsonWriteErrorCode::InvalidUtf8, offset});
    }
    out.push_back('"');
    for (const char character : text) {
        switch (character) {
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
        default: {
            const auto code = static_cast<unsigned char>(character);
            if (code < 0x20U) {
                out += "\\u00";
                out.push_back(kHexDigits[code >> 4U]);
                out.push_back(kHexDigits[code & 0x0FU]);
            } else {
                out.push_back(character);
            }
            break;
        }
        }
    }
    out.push_back('"');
    return {};
}

std::expected<void, JsonWriteError> append_number(std::string& out, const JsonValue& value) {
    if (const auto integer = value.as_integer(); integer.has_value()) {
        char buffer[24];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *integer);
        out.append(buffer, result.ptr);
        return {};
    }
    const double number = value.as_number().value_or(0.0);
    if (!std::isfinite(number)) {
        return std::unexpected(JsonWriteError{JsonWriteErrorCode::NonFiniteNumber, 0});
    }
    char buffer[40];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::general);
    if (result.ec != std::errc{}) {
        return std::unexpected(JsonWriteError{JsonWriteErrorCode::NonFiniteNumber, 0});
    }
    out.append(buffer, result.ptr);
    return {};
}

std::expected<void, JsonWriteError> append_value(std::string& out, const JsonValue& value) {
    switch (value.type()) {
    case JsonType::Null:
        out += "null";
        return {};
    case JsonType::Bool:
        out += (value.as_bool().value_or(false) ? "true" : "false");
        return {};
    case JsonType::Number:
        return append_number(out, value);
    case JsonType::String:
        return append_string(out, value.as_string().value_or(std::string_view{}), out.size());
    case JsonType::Array: {
        out.push_back('[');
        bool first = true;
        for (const JsonValue& element : *value.as_array()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            if (auto appended = append_value(out, element); !appended) {
                return appended;
            }
        }
        out.push_back(']');
        return {};
    }
    case JsonType::Object: {
        out.push_back('{');
        bool first = true;
        for (const JsonValue::Member& member : *value.as_object()) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            if (auto appended = append_string(out, member.first, out.size()); !appended) {
                return appended;
            }
            out.push_back(':');
            if (auto appended = append_value(out, member.second); !appended) {
                return appended;
            }
        }
        out.push_back('}');
        return {};
    }
    }
    return {};
}

} // namespace

std::expected<void, JsonWriteError> append_json(std::string& out, const JsonValue& value) {
    const std::size_t start = out.size();
    if (auto appended = append_value(out, value); !appended) {
        out.resize(start);
        return appended;
    }
    return {};
}

std::expected<std::string, JsonWriteError> to_json(const JsonValue& value) {
    std::string out;
    out.reserve(128);
    if (auto appended = append_value(out, value); !appended) {
        return std::unexpected(appended.error());
    }
    return out;
}

} // namespace tessera::harness
