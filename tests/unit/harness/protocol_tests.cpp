// SPDX-License-Identifier: MIT

#include "tessera/harness/json_value.hpp"
#include "tessera/harness/protocol.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using tessera::harness::decode_request;
using tessera::harness::decode_response;
using tessera::harness::encode_request;
using tessera::harness::encode_response;
using tessera::harness::JsonValue;
using tessera::harness::kProtocolVersion;
using tessera::harness::make_error_response;
using tessera::harness::protocol_error_code_from_string;
using tessera::harness::ProtocolError;
using tessera::harness::ProtocolErrorCode;
using tessera::harness::Request;
using tessera::harness::RequestId;
using tessera::harness::Response;
using tessera::harness::to_string;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harness_protocol: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string encoded(const std::expected<std::string, ProtocolError>& text) {
    return text.has_value() ? *text : std::string("<encode failed>");
}

std::optional<ProtocolErrorCode> request_error(std::string_view line) {
    const auto request = decode_request(line);
    if (request.has_value()) {
        return std::nullopt;
    }
    return request.error().code;
}

std::optional<ProtocolErrorCode> response_error(std::string_view line) {
    const auto response = decode_response(line);
    if (response.has_value()) {
        return std::nullopt;
    }
    return response.error().code;
}

void check_request_envelope() {
    // The spec example: flat arguments, numeric id, no version field.
    const auto request = decode_request(R"({"id":3,"cmd":"click","uiId":"222222222222222222"})");
    expect(request.has_value(), "spec example decodes");
    expect(request->id == 3, "numeric id");
    expect(request->command == "click", "command name");
    expect(request->arguments.find("uiId") != nullptr, "argument present");
    expect(request->arguments.find("uiId")->as_string() ==
               std::optional<std::string_view>{"222222222222222222"},
           "argument value");

    // Canonical request bytes: version, decimal-string id, cmd, arguments in wire order.
    Request outbound;
    outbound.id = 3;
    outbound.command = "click";
    outbound.arguments.set("uiId", JsonValue::string("222222222222222222"));
    const std::string text = encoded(encode_request(outbound));
    expect(text == R"({"protocolVersion":1,"id":"3","cmd":"click","uiId":"222222222222222222"})",
           "canonical request bytes");
    expect(encoded(encode_request(outbound)) == text, "request bytes are stable");

    // Round-trip through the decoder preserves the envelope and the argument order.
    const auto round_trip = decode_request(text);
    expect(round_trip.has_value() && *round_trip == outbound, "request round-trip");

    const auto ordered = decode_request(R"({"id":1,"cmd":"action","b":1,"a":2})");
    expect(ordered.has_value(), "argument order decodes");
    expect(encoded(encode_request(*ordered)) ==
               R"({"protocolVersion":1,"id":"1","cmd":"action","b":1,"a":2})",
           "argument order is preserved");

    // uint64 correlation ids above 2^53 survive because ids are decimal strings.
    const auto maximum = decode_request(R"({"id":"18446744073709551615","cmd":"snapshot"})");
    expect(maximum.has_value() && maximum->id == std::numeric_limits<RequestId>::max(),
           "uint64 max id");
    expect(encoded(encode_request(*maximum)) ==
               R"({"protocolVersion":1,"id":"18446744073709551615","cmd":"snapshot"})",
           "uint64 max id writes as a string");

    // A small JSON number is accepted for hand-written clients; a fractional or negative one is
    // not an id.
    expect(decode_request(R"({"id":7,"cmd":"snapshot"})")->id == 7, "exact numeric id");
    expect(request_error(R"({"id":1.5,"cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "fractional id rejected");
    expect(request_error(R"({"id":-1,"cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "negative id rejected");
    expect(request_error(R"({"id":"01","cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "non-canonical decimal id rejected");
    expect(request_error(R"({"id":"","cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "empty id rejected");
    expect(request_error(R"({"id":"abc","cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "non-numeric id rejected");
    expect(request_error(R"({"id":"18446744073709551616","cmd":"snapshot"})") ==
               ProtocolErrorCode::MalformedRequest,
           "id overflow rejected");
    expect(request_error(R"({"cmd":"snapshot"})") == ProtocolErrorCode::MalformedRequest,
           "missing id rejected");
    expect(request_error(R"({"id":1})") == ProtocolErrorCode::MalformedRequest,
           "missing cmd rejected");
    expect(request_error(R"({"id":1,"cmd":""})") == ProtocolErrorCode::MalformedRequest,
           "empty cmd rejected");
    expect(request_error(R"({"id":1,"cmd":5})") == ProtocolErrorCode::MalformedRequest,
           "non-string cmd rejected");

    // Shape and JSON failures are reported as malformed, never as an exception.
    expect(request_error("[]") == ProtocolErrorCode::MalformedRequest, "array rejected");
    expect(request_error("null") == ProtocolErrorCode::MalformedRequest, "null rejected");
    expect(request_error("{") == ProtocolErrorCode::MalformedRequest, "truncated json rejected");
    expect(decode_request(R"({"id":1,"cmd":"snapshot","id":2})").error().code ==
               ProtocolErrorCode::MalformedRequest,
           "duplicate id rejected");

    // Versioning: explicit v1 passes, v2 is rejected and the recovered id is echoed.
    const auto versioned = decode_request(R"({"protocolVersion":1,"id":4,"cmd":"snapshot"})");
    expect(versioned.has_value(), "explicit v1 decodes");
    const auto future = decode_request(R"({"id":9,"protocolVersion":2,"cmd":"snapshot"})");
    expect(!future.has_value() && future.error().code == ProtocolErrorCode::UnsupportedVersion,
           "future version rejected");
    expect(future.error().request_id == std::optional<RequestId>{9}, "version error echoes id");
    expect(!decode_request(R"({"id":9,"protocolVersion":"1","cmd":"snapshot"})").has_value(),
           "string version rejected");

    // Encoding rejects an argument that would shadow an envelope field, and a non-object
    // argument bag.
    Request shadowed;
    shadowed.id = 1;
    shadowed.command = "snapshot";
    shadowed.arguments.set("cmd", JsonValue::string("click"));
    expect(encode_request(shadowed).error().code == ProtocolErrorCode::InvalidArgument,
           "reserved argument rejected");
    Request not_an_object;
    not_an_object.id = 1;
    not_an_object.command = "snapshot";
    not_an_object.arguments = JsonValue::array();
    expect(encode_request(not_an_object).error().code == ProtocolErrorCode::InvalidArgument,
           "non-object arguments rejected");
    Request empty_command;
    empty_command.id = 1;
    expect(encode_request(empty_command).error().code == ProtocolErrorCode::InvalidArgument,
           "empty cmd cannot be encoded");
}

void check_response_envelope() {
    Response ok;
    ok.id = 7;
    ok.ok = true;
    ok.result = JsonValue::array();
    ok.elapsed_us = 42;
    const std::string ok_text = encoded(encode_response(ok));
    expect(ok_text == R"({"protocolVersion":1,"id":"7","ok":true,"result":[],"elapsedUs":42})",
           "canonical ok response bytes");
    expect(encoded(encode_response(ok)) == ok_text, "response bytes are stable");
    const auto decoded_ok = decode_response(ok_text);
    expect(decoded_ok.has_value() && *decoded_ok == ok, "ok response round-trip");

    Response failure;
    failure.id = 7;
    failure.ok = false;
    failure.error.code = ProtocolErrorCode::UiIdNotFound;
    failure.error.message = "no node";
    failure.error.detail = "987";
    const std::string error_text = encoded(encode_response(failure));
    expect(
        error_text ==
            R"({"protocolVersion":1,"id":"7","ok":false,"error":{"code":"uiIdNotFound","message":"no node","detail":"987"},"elapsedUs":0})",
        "canonical error response bytes");
    const auto decoded_error = decode_response(error_text);
    expect(decoded_error.has_value() && *decoded_error == failure, "error response round-trip");

    // An empty detail is omitted rather than emitted as "".
    Response minimal;
    minimal.id = 1;
    minimal.ok = false;
    minimal.error.code = ProtocolErrorCode::Timeout;
    minimal.error.message = "too slow";
    expect(
        encoded(encode_response(minimal)) ==
            R"({"protocolVersion":1,"id":"1","ok":false,"error":{"code":"timeout","message":"too slow"},"elapsedUs":0})",
        "empty detail omitted");

    // A result-less success decodes as null result; elapsedUs defaults to 0.
    const auto bare = decode_response(R"({"id":"1","ok":true})");
    expect(bare.has_value() && bare->result.is_null() && bare->elapsed_us == 0,
           "minimal ok response");

    // Shape violations are rejected instead of normalized.
    expect(response_error(R"({"id":"1","ok":true,"error":{"code":"timeout","message":"x"}})") ==
               ProtocolErrorCode::MalformedRequest,
           "ok response with error rejected");
    expect(response_error(R"({"id":"1","ok":false})") == ProtocolErrorCode::MalformedRequest,
           "failure without error rejected");
    expect(response_error(
               R"({"id":"1","ok":false,"error":{"code":"timeout","message":"x"},"result":1})") ==
               ProtocolErrorCode::MalformedRequest,
           "failure with result rejected");
    expect(response_error(R"({"id":"1","ok":true,"extra":1})") ==
               ProtocolErrorCode::MalformedRequest,
           "unknown response field rejected");
    expect(response_error(R"({"id":"1","ok":true,"error":{"code":"x","message":"y","z":1}})") ==
               ProtocolErrorCode::MalformedRequest,
           "unknown error field rejected");
    expect(response_error(R"({"id":"1","ok":true,"error":5})") ==
               ProtocolErrorCode::MalformedRequest,
           "non-object error rejected");
    expect(response_error(R"({"ok":true})") == ProtocolErrorCode::MalformedRequest,
           "missing response id rejected");
    expect(response_error(R"({"id":"1"})") == ProtocolErrorCode::MalformedRequest,
           "missing ok rejected");
    expect(response_error(R"({"id":"1","ok":"yes"})") == ProtocolErrorCode::MalformedRequest,
           "non-boolean ok rejected");
    expect(response_error(R"({"id":"1","ok":true,"elapsedUs":-1})") ==
               ProtocolErrorCode::MalformedRequest,
           "negative elapsed rejected");
    expect(response_error(R"({"id":"1","ok":true,"elapsedUs":1.5})") ==
               ProtocolErrorCode::MalformedRequest,
           "fractional elapsed rejected");
    expect(response_error(R"({"protocolVersion":2,"id":"1","ok":true})") ==
               ProtocolErrorCode::UnsupportedVersion,
           "future response version rejected");

    Response negative_elapsed;
    negative_elapsed.id = 1;
    negative_elapsed.elapsed_us = -1;
    expect(encode_response(negative_elapsed).error().code == ProtocolErrorCode::InvalidArgument,
           "negative elapsed cannot be encoded");
}

void check_error_codes_and_helpers() {
    for (const ProtocolErrorCode code :
         {ProtocolErrorCode::MalformedRequest, ProtocolErrorCode::UnsupportedVersion,
          ProtocolErrorCode::UnknownCommand, ProtocolErrorCode::InvalidArgument,
          ProtocolErrorCode::UiIdNotFound, ProtocolErrorCode::Timeout,
          ProtocolErrorCode::Internal}) {
        expect(protocol_error_code_from_string(to_string(code)) == code, "error code round-trip");
    }
    expect(protocol_error_code_from_string("notACode") == ProtocolErrorCode::Internal,
           "unknown error code degrades to internal");
    expect(to_string(ProtocolErrorCode::MalformedRequest) == "malformedRequest",
           "wire spelling is camelCase");
    expect(kProtocolVersion == 1, "v1 protocol version");

    ProtocolError error;
    error.code = ProtocolErrorCode::InvalidArgument;
    error.message = "bad uiId";
    error.request_id = 11;
    const Response response = make_error_response(0, error);
    expect(response.id == 11, "recovered id wins");
    expect(!response.ok && response.error.code == ProtocolErrorCode::InvalidArgument,
           "error response carries the failure");
    expect(!response.error.request_id.has_value(), "inner correlation id is not re-emitted");

    ProtocolError untracked;
    untracked.code = ProtocolErrorCode::Internal;
    untracked.message = "boom";
    expect(make_error_response(5, untracked).id == 5, "fallback id");
    expect(
        encoded(encode_response(make_error_response(5, untracked))) ==
            R"({"protocolVersion":1,"id":"5","ok":false,"error":{"code":"internal","message":"boom"},"elapsedUs":0})",
        "internal error envelope bytes");
}

} // namespace

int main() {
    check_request_envelope();
    check_response_envelope();
    check_error_codes_and_helpers();
    if (failures != 0) {
        std::cerr << "harness_protocol: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
