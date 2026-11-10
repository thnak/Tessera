// SPDX-License-Identifier: MIT

#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::harness {
namespace {

constexpr std::string_view kVersionField = "protocolVersion";
constexpr std::string_view kIdField = "id";
constexpr std::string_view kCommandField = "cmd";
constexpr std::string_view kOkField = "ok";
constexpr std::string_view kResultField = "result";
constexpr std::string_view kErrorField = "error";
constexpr std::string_view kElapsedField = "elapsedUs";
constexpr std::string_view kErrorCodeField = "code";
constexpr std::string_view kErrorMessageField = "message";
constexpr std::string_view kErrorDetailField = "detail";

[[nodiscard]] std::unexpected<ProtocolError> fail(ProtocolErrorCode code, std::string message,
                                                  std::string detail = {},
                                                  std::optional<RequestId> request_id = {}) {
    return std::unexpected(
        ProtocolError{code, std::move(message), std::move(detail), std::move(request_id)});
}

[[nodiscard]] std::unexpected<ProtocolError> malformed(std::string message, std::string detail = {},
                                                       std::optional<RequestId> id = {}) {
    return fail(ProtocolErrorCode::MalformedRequest, std::move(message), std::move(detail),
                std::move(id));
}

// Canonical decimal text for a uint64: no sign, no leading zeros, "0" for zero.
[[nodiscard]] std::string decimal(RequestId id) {
    return std::to_string(id);
}

// Wire ids are decimal strings; a JSON number is accepted for hand-written clients but must be a
// non-negative exact integer.
[[nodiscard]] bool read_request_id(const JsonValue& value, RequestId& out) {
    if (const auto text = value.as_string(); text.has_value()) {
        return parse_decimal_id(*text, out);
    }
    if (const auto integer = value.as_integer(); integer.has_value() && *integer >= 0) {
        out = static_cast<RequestId>(*integer);
        return true;
    }
    return false;
}

[[nodiscard]] bool is_reserved_request_field(std::string_view key) {
    return key == kVersionField || key == kIdField || key == kCommandField;
}

[[nodiscard]] bool read_protocol_version(const JsonValue& value, ProtocolError& error,
                                         std::optional<RequestId> id) {
    const auto version = value.as_integer();
    if (!version.has_value()) {
        error =
            ProtocolError{ProtocolErrorCode::MalformedRequest, "protocolVersion must be an integer",
                          std::string(kVersionField), std::move(id)};
        return false;
    }
    if (*version != kProtocolVersion) {
        error = ProtocolError{ProtocolErrorCode::UnsupportedVersion, "unsupported protocol version",
                              "supported=" + std::to_string(kProtocolVersion) +
                                  " received=" + std::to_string(*version),
                              std::move(id)};
        return false;
    }
    return true;
}

[[nodiscard]] std::expected<JsonValue, ProtocolError> parse_document(std::string_view line) {
    auto parsed = parse_json(line);
    if (!parsed) {
        return malformed("line is not valid JSON",
                         "offset=" + std::to_string(parsed.error().offset));
    }
    return std::move(*parsed);
}

} // namespace

std::string_view to_string(ProtocolErrorCode code) noexcept {
    switch (code) {
    case ProtocolErrorCode::MalformedRequest:
        return "malformedRequest";
    case ProtocolErrorCode::UnsupportedVersion:
        return "unsupportedVersion";
    case ProtocolErrorCode::UnknownCommand:
        return "unknownCommand";
    case ProtocolErrorCode::InvalidArgument:
        return "invalidArgument";
    case ProtocolErrorCode::UiIdNotFound:
        return "uiIdNotFound";
    case ProtocolErrorCode::Timeout:
        return "timeout";
    case ProtocolErrorCode::Internal:
        return "internal";
    }
    return "internal";
}

ProtocolErrorCode protocol_error_code_from_string(std::string_view text) noexcept {
    for (const ProtocolErrorCode code :
         {ProtocolErrorCode::MalformedRequest, ProtocolErrorCode::UnsupportedVersion,
          ProtocolErrorCode::UnknownCommand, ProtocolErrorCode::InvalidArgument,
          ProtocolErrorCode::UiIdNotFound, ProtocolErrorCode::Timeout,
          ProtocolErrorCode::Internal}) {
        if (to_string(code) == text) {
            return code;
        }
    }
    return ProtocolErrorCode::Internal;
}

bool parse_decimal_id(std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

std::expected<Request, ProtocolError> decode_request(std::string_view line) {
    auto document = parse_document(line);
    if (!document) {
        return std::unexpected(document.error());
    }
    const JsonValue::Object* members = document->as_object();
    if (members == nullptr) {
        return malformed("request must be a JSON object");
    }
    Request request;
    bool has_id = false;
    bool has_command = false;
    std::optional<RequestId> recovered_id;
    for (const JsonValue::Member& member : *members) {
        if (member.first == kVersionField) {
            ProtocolError error;
            if (!read_protocol_version(member.second, error, recovered_id)) {
                return std::unexpected(std::move(error));
            }
            continue;
        }
        if (member.first == kIdField) {
            if (has_id) {
                return malformed("duplicate id", std::string(kIdField), recovered_id);
            }
            if (!read_request_id(member.second, request.id)) {
                return malformed("id must be an unsigned decimal string", std::string(kIdField),
                                 recovered_id);
            }
            has_id = true;
            recovered_id = request.id;
            continue;
        }
        if (member.first == kCommandField) {
            const auto command = member.second.as_string();
            if (!command.has_value() || command->empty()) {
                return malformed("cmd must be a non-empty string", std::string(kCommandField),
                                 recovered_id);
            }
            request.command = std::string(*command);
            has_command = true;
            continue;
        }
        request.arguments.set(member.first, member.second);
    }
    if (!has_id) {
        return malformed("request has no id", std::string(kIdField), recovered_id);
    }
    if (!has_command) {
        return malformed("request has no cmd", std::string(kCommandField), recovered_id);
    }
    return request;
}

std::expected<std::string, ProtocolError> encode_request(const Request& request) {
    if (request.command.empty()) {
        return fail(ProtocolErrorCode::InvalidArgument, "cmd must not be empty",
                    std::string(kCommandField), request.id);
    }
    const JsonValue::Object* arguments = request.arguments.as_object();
    if (arguments == nullptr) {
        return fail(ProtocolErrorCode::InvalidArgument, "arguments must be an object",
                    std::string(kCommandField), request.id);
    }
    JsonValue::Object members;
    members.reserve(arguments->size() + 3);
    members.emplace_back(std::string(kVersionField), JsonValue::integer(kProtocolVersion));
    members.emplace_back(std::string(kIdField), JsonValue::string(decimal(request.id)));
    members.emplace_back(std::string(kCommandField), JsonValue::string(request.command));
    for (const JsonValue::Member& argument : *arguments) {
        if (is_reserved_request_field(argument.first)) {
            return fail(ProtocolErrorCode::InvalidArgument, "argument redefines an envelope field",
                        argument.first, request.id);
        }
        members.push_back(argument);
    }
    auto text = to_json(JsonValue::object(std::move(members)));
    if (!text) {
        return fail(ProtocolErrorCode::Internal, "request cannot be serialized", {}, request.id);
    }
    return std::move(*text);
}

std::expected<Response, ProtocolError> decode_response(std::string_view line) {
    auto document = parse_document(line);
    if (!document) {
        return std::unexpected(document.error());
    }
    const JsonValue::Object* members = document->as_object();
    if (members == nullptr) {
        return malformed("response must be a JSON object");
    }
    Response response;
    bool has_id = false;
    bool has_ok = false;
    bool has_result = false;
    bool has_error = false;
    for (const JsonValue::Member& member : *members) {
        if (member.first == kVersionField) {
            ProtocolError error;
            if (!read_protocol_version(member.second, error, std::nullopt)) {
                return std::unexpected(std::move(error));
            }
            continue;
        }
        if (member.first == kIdField) {
            if (has_id || !read_request_id(member.second, response.id)) {
                return malformed("id must be an unsigned decimal string", std::string(kIdField));
            }
            has_id = true;
            continue;
        }
        if (member.first == kOkField) {
            const auto ok = member.second.as_bool();
            if (has_ok || !ok.has_value()) {
                return malformed("ok must be a boolean", std::string(kOkField));
            }
            response.ok = *ok;
            has_ok = true;
            continue;
        }
        if (member.first == kResultField) {
            if (has_result) {
                return malformed("duplicate result", std::string(kResultField));
            }
            response.result = member.second;
            has_result = true;
            continue;
        }
        if (member.first == kErrorField) {
            const JsonValue::Object* error_members = member.second.as_object();
            if (has_error || error_members == nullptr) {
                return malformed("error must be an object", std::string(kErrorField));
            }
            const JsonValue* code = member.second.find(kErrorCodeField);
            const auto message = member.second.find(kErrorMessageField);
            if (code == nullptr || !code->as_string().has_value()) {
                return malformed("error.code must be a string", std::string(kErrorCodeField));
            }
            if (message == nullptr || !message->as_string().has_value()) {
                return malformed("error.message must be a string", std::string(kErrorMessageField));
            }
            response.error.code = protocol_error_code_from_string(*code->as_string());
            response.error.message = std::string(*message->as_string());
            if (const JsonValue* detail = member.second.find(kErrorDetailField);
                detail != nullptr) {
                const auto text = detail->as_string();
                if (!text.has_value()) {
                    return malformed("error.detail must be a string",
                                     std::string(kErrorDetailField));
                }
                response.error.detail = std::string(*text);
            }
            for (const JsonValue::Member& entry : *error_members) {
                if (entry.first != kErrorCodeField && entry.first != kErrorMessageField &&
                    entry.first != kErrorDetailField) {
                    return malformed("unknown error field", entry.first);
                }
            }
            has_error = true;
            continue;
        }
        if (member.first == kElapsedField) {
            const auto elapsed = member.second.as_integer();
            if (!elapsed.has_value() || *elapsed < 0) {
                return malformed("elapsedUs must be a non-negative integer",
                                 std::string(kElapsedField));
            }
            response.elapsed_us = *elapsed;
            continue;
        }
        return malformed("unknown response field", member.first);
    }
    if (!has_id) {
        return malformed("response has no id", std::string(kIdField));
    }
    if (!has_ok) {
        return malformed("response has no ok", std::string(kOkField));
    }
    if (response.ok && has_error) {
        return malformed("an ok response must not carry error", std::string(kErrorField));
    }
    if (!response.ok && !has_error) {
        return malformed("a failed response must carry error", std::string(kErrorField));
    }
    if (!response.ok && has_result) {
        return malformed("a failed response must not carry result", std::string(kResultField));
    }
    return response;
}

std::expected<std::string, ProtocolError> encode_response(const Response& response) {
    if (response.elapsed_us < 0) {
        return fail(ProtocolErrorCode::InvalidArgument, "elapsedUs must not be negative",
                    std::string(kElapsedField), response.id);
    }
    JsonValue::Object members;
    members.reserve(5);
    members.emplace_back(std::string(kVersionField), JsonValue::integer(kProtocolVersion));
    members.emplace_back(std::string(kIdField), JsonValue::string(decimal(response.id)));
    members.emplace_back(std::string(kOkField), JsonValue::boolean(response.ok));
    if (response.ok) {
        members.emplace_back(std::string(kResultField), response.result);
    } else {
        JsonValue::Object error;
        error.emplace_back(std::string(kErrorCodeField),
                           JsonValue::string(std::string(to_string(response.error.code))));
        error.emplace_back(std::string(kErrorMessageField),
                           JsonValue::string(response.error.message));
        if (!response.error.detail.empty()) {
            error.emplace_back(std::string(kErrorDetailField),
                               JsonValue::string(response.error.detail));
        }
        members.emplace_back(std::string(kErrorField), JsonValue::object(std::move(error)));
    }
    members.emplace_back(std::string(kElapsedField), JsonValue::integer(response.elapsed_us));
    auto text = to_json(JsonValue::object(std::move(members)));
    if (!text) {
        return fail(ProtocolErrorCode::Internal, "response cannot be serialized", {}, response.id);
    }
    return std::move(*text);
}

Response make_error_response(RequestId id, ProtocolError error) {
    const RequestId correlation = error.request_id.value_or(id);
    Response response;
    response.id = correlation;
    response.ok = false;
    response.elapsed_us = 0;
    error.request_id.reset();
    response.error = std::move(error);
    return response;
}

} // namespace tessera::harness
