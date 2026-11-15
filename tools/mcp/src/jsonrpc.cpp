// SPDX-License-Identifier: MIT

#include "jsonrpc.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/harness/json_write.hpp"

#include <string>
#include <utility>

namespace tessera::mcp {
namespace {

using tessera::harness::parse_json;
using tessera::harness::to_json;

std::unexpected<DecodeFailure> fail(RpcErrorCode code, std::string_view message,
                                    std::string_view data_code, RpcId id = {}) {
    return std::unexpected(DecodeFailure{RpcError{code, message, data_code}, std::move(id)});
}

// The envelope members of a request object. `id` accepts a string, a number, or null (the
// specification allows null although it is discouraged); any other type is an invalid request.
bool read_id(const JsonValue& object, RpcId& id) {
    const JsonValue* value = object.find("id");
    if (value == nullptr) {
        return true;
    }
    if (!value->is_string() && !value->is_number() && !value->is_null()) {
        return false;
    }
    id.present = true;
    id.value = *value;
    return true;
}

} // namespace

std::expected<IncomingMessage, DecodeFailure> decode_message(std::string_view line) {
    const auto parsed = parse_json(line);
    if (!parsed) {
        return fail(RpcErrorCode::ParseError, "the line is not valid JSON", "");
    }
    const JsonValue& object = *parsed;
    if (!object.is_object()) {
        // A batch array and a bare scalar are both "this is not a JSON-RPC message".
        return fail(RpcErrorCode::InvalidRequest,
                    "a message must be one JSON object; batching is not supported", "");
    }

    IncomingMessage message;
    if (!read_id(object, message.id)) {
        // The id could not be read, so the failure has no request to correlate with.
        return fail(RpcErrorCode::InvalidRequest, "`id` must be a string, a number, or null", "");
    }

    const JsonValue* version = object.find("jsonrpc");
    if (version == nullptr || !version->is_string() || *version->as_string() != kJsonRpcVersion) {
        return fail(RpcErrorCode::InvalidRequest, "`jsonrpc` must be \"2.0\"", "",
                    std::move(message.id));
    }
    const JsonValue* method = object.find("method");
    if (method == nullptr || !method->is_string() || method->as_string()->empty()) {
        return fail(RpcErrorCode::InvalidRequest, "`method` must be a non-empty string", "",
                    std::move(message.id));
    }
    message.method = std::string(*method->as_string());

    // A response to a server-initiated request is not something this server can use: it issues no
    // requests, so a `result`/`error` member with no `method` was already rejected above.
    if (const JsonValue* params = object.find("params"); params != nullptr) {
        if (!params->is_object()) {
            // Only object params exist in MCP today. Recording the type lets the method report
            // InvalidParams instead of pretending it saw an empty object.
            message.params_were_object = params->is_null();
            if (!params->is_null()) {
                return fail(RpcErrorCode::InvalidParams, "`params` must be an object", "",
                            std::move(message.id));
            }
        } else {
            message.params = *params;
        }
    }
    return message;
}

std::expected<std::string, std::string_view> encode_result(const RpcId& id,
                                                           const JsonValue& result) {
    JsonValue document = JsonValue::object();
    document.set("jsonrpc", JsonValue::string(std::string(kJsonRpcVersion)));
    document.set("id", id.present ? id.value : JsonValue());
    document.set("result", result);
    auto text = to_json(document);
    if (!text) {
        // Only a non-finite number or invalid UTF-8 reaches this, and a tool result is built from
        // compiler output and canonical JSON, so it is a programming error rather than peer input.
        return std::unexpected(std::string_view("the result cannot be encoded as JSON"));
    }
    return std::move(*text);
}

std::expected<std::string, std::string_view> encode_error(const RpcId& id, RpcError error) {
    JsonValue body = JsonValue::object();
    body.set("code", JsonValue::integer(static_cast<std::int64_t>(error.code)));
    body.set("message", JsonValue::string(std::string(error.message)));
    if (!error.data_code.empty()) {
        JsonValue data = JsonValue::object();
        data.set("code", JsonValue::string(std::string(error.data_code)));
        body.set("data", std::move(data));
    }

    JsonValue document = JsonValue::object();
    document.set("jsonrpc", JsonValue::string(std::string(kJsonRpcVersion)));
    document.set("id", id.present ? id.value : JsonValue());
    document.set("error", std::move(body));
    auto text = to_json(document);
    if (!text) {
        return std::unexpected(std::string_view("the error cannot be encoded as JSON"));
    }
    return std::move(*text);
}

std::string encode_decode_failure(const DecodeFailure& failure) {
    auto text = encode_error(failure.id, failure.error);
    if (text) {
        return std::move(*text);
    }
    // The failure body is built from constants and a code number, so this is unreachable; a
    // request must still be answered rather than dropped, so fall back to the shortest valid error.
    return R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"internal error"}})";
}

} // namespace tessera::mcp
