// SPDX-License-Identifier: MIT

#pragma once

#include "mcp_protocol.hpp"
#include "tessera/harness/json_value.hpp"

#include <expected>
#include <string>
#include <string_view>

// The JSON-RPC 2.0 half of MCP: decode one line into a request or notification, and encode one
// result or error. The repository's canonical value type (`harness::JsonValue`) is the in-memory
// form and its canonical writer produces the bytes, so this unit adds the envelope rules only and
// never a JSON implementation of its own (no third-party dependency; AGENTS.md rule 2 and 10).
namespace tessera::mcp {

using tessera::harness::JsonValue;

// A JSON-RPC id: absent (a notification), or a string/number/null that must be echoed back
// unchanged. Keeping the decoded value is what makes the echo exact: rewriting 7 as "7" would break
// correlation for a client that sent a number.
struct RpcId {
    bool present = false;
    JsonValue value; // Null when the client sent `"id": null`

    [[nodiscard]] bool is_null() const noexcept {
        return present && value.is_null();
    }
};

// One decoded incoming message. The server issues no requests of its own (MCP is client-driven in
// this adapter), so a message is always a request or a notification.
struct IncomingMessage {
    RpcId id;
    std::string method;
    JsonValue params{JsonValue::object()};
    // `"params": null` and a missing `params` are both "no parameters"; this flag is only used to
    // report a non-object params value as InvalidParams for a method that needs an object.
    bool params_were_object = true;

    [[nodiscard]] bool is_request() const noexcept {
        return !method.empty() && id.present;
    }
    [[nodiscard]] bool is_notification() const noexcept {
        return !method.empty() && !id.present;
    }
};

// Why a line could not be decoded. `id` is echoed when the object carried a usable one, because
// JSON-RPC requires a parse/invalid-request error to correlate with the request when possible.
struct DecodeFailure {
    RpcError error;
    RpcId id;
};

// Decodes one line (no terminator). Everything the specification marks optional on the way in is
// optional here; everything it marks required is enforced, including `"jsonrpc": "2.0"`. A
// top-level array (JSON-RPC batch) is rejected: MCP removed batching in 2025-06-18 and a client
// that sends one is told so instead of having half a batch executed.
[[nodiscard]] std::expected<IncomingMessage, DecodeFailure> decode_message(std::string_view line);

// One successful response line. `id` is the request's id verbatim.
[[nodiscard]] std::expected<std::string, std::string_view> encode_result(const RpcId& id,
                                                                         const JsonValue& result);

// One error response line. `id` is the request's id when one was readable, otherwise null (a parse
// error has no request to correlate with).
[[nodiscard]] std::expected<std::string, std::string_view> encode_error(const RpcId& id,
                                                                        RpcError error);

// Convenience: an error response for a line that never decoded.
[[nodiscard]] std::string encode_decode_failure(const DecodeFailure& failure);

} // namespace tessera::mcp
