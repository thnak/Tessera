// SPDX-License-Identifier: MIT

#include "command_internal.hpp"

#include <string>
#include <utility>

namespace tessera::harness::detail {

std::unexpected<ProtocolError> invalid_argument(std::string message, std::string detail) {
    return std::unexpected(ProtocolError{ProtocolErrorCode::InvalidArgument, std::move(message),
                                         std::move(detail), std::nullopt});
}

std::optional<ProtocolError> reject_unknown(const JsonValue& value,
                                            std::initializer_list<std::string_view> allowed) {
    const JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return ProtocolError{
            ProtocolErrorCode::InvalidArgument, "payload must be an object", {}, std::nullopt};
    }
    for (const JsonValue::Member& member : *members) {
        bool known = false;
        for (const std::string_view key : allowed) {
            known = known || key == member.first;
        }
        if (!known) {
            return ProtocolError{ProtocolErrorCode::InvalidArgument, "unknown field", member.first,
                                 std::nullopt};
        }
    }
    return std::nullopt;
}

bool read_ui_id(const JsonValue& value, UiId& out) {
    if (const auto text = value.as_string(); text.has_value()) {
        return parse_decimal_id(*text, out);
    }
    if (const auto integer = value.as_integer(); integer.has_value() && *integer >= 0) {
        out = static_cast<UiId>(*integer);
        return true;
    }
    return false;
}

std::optional<std::string> read_string(const JsonValue* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto text = value->as_string();
    if (!text.has_value()) {
        return std::nullopt;
    }
    return std::string(*text);
}

} // namespace tessera::harness::detail
