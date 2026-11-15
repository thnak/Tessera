// SPDX-License-Identifier: MIT
//
// Shared helpers for the MCP adapter tests: one `tools/call` through a real session, and a few
// accessors that read a decoded response the way a client does. Header-only with no shared state,
// so each test links its own copy (the same pattern as tests/unit/harness/engine_host_support.hpp).

#pragma once

#include "session.hpp"
#include "tessera/harness/json_parse.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace mcp_test {

namespace harness = tessera::harness;
namespace mcp = tessera::mcp;

inline int failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << "\n";
        ++failures;
    }
    return condition;
}

// Decodes a response line, reporting a non-JSON response as a failure rather than aborting: one bad
// line should not hide the other assertions of the same run.
inline harness::JsonValue decode(std::string_view line, std::string_view who) {
    auto parsed = harness::parse_json(line);
    if (!parsed) {
        std::cerr << who << ": response is not strict JSON: " << line << "\n";
        ++failures;
        return harness::JsonValue::object();
    }
    return *parsed;
}

inline std::string string_at(const harness::JsonValue& object, std::string_view key) {
    const harness::JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return std::string(*value->as_string());
}

// The handshake every tool test needs before `tools/list` or `tools/call` is accepted.
inline void greet(mcp::McpSession& session, std::string_view who) {
    auto reply = session.handle_line(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp_test","version":"1"}}})");
    expect(reply.has_value() && reply->answered,
           std::string(who) + ": the session accepts the handshake");
}

// One `tools/call` through the session, returning the decoded response object. `arguments_json` is
// a raw JSON object, so a test reads as the call the client would write.
inline harness::JsonValue call(mcp::McpSession& session, std::string_view name,
                               std::string_view arguments_json, std::string_view who,
                               std::uint64_t id = 1) {
    const std::string line = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                             R"(,"method":"tools/call","params":{"name":")" + std::string(name) +
                             R"(","arguments":)" + std::string(arguments_json) + "}}";
    auto reply = session.handle_line(line);
    if (!reply) {
        std::cerr << who << ": the session could not answer a tools/call\n";
        ++failures;
        return harness::JsonValue::object();
    }
    return decode(reply->reply, who);
}

// The text content block of a tool result.
inline std::string result_text(const harness::JsonValue& response) {
    const harness::JsonValue* result = response.find("result");
    const harness::JsonValue* content = result == nullptr ? nullptr : result->find("content");
    const harness::JsonValue::Array* items = content == nullptr ? nullptr : content->as_array();
    if (items == nullptr || items->empty()) {
        return {};
    }
    return string_at(items->front(), "text");
}

inline bool result_is_error(const harness::JsonValue& response) {
    const harness::JsonValue* result = response.find("result");
    const harness::JsonValue* flag = result == nullptr ? nullptr : result->find("isError");
    return flag != nullptr && flag->as_bool() == std::optional<bool>(true);
}

// The `error.data.code` of a protocol error response, or empty when there is no typed code.
inline std::string error_data_code(const harness::JsonValue& response) {
    const harness::JsonValue* error = response.find("error");
    const harness::JsonValue* data = error == nullptr ? nullptr : error->find("data");
    return data == nullptr ? std::string() : string_at(*data, "code");
}

// The JSON-RPC `error.code` of an error response, or 0 when the response is not an error.
inline std::int64_t error_code(const harness::JsonValue& response) {
    const harness::JsonValue* error = response.find("error");
    const harness::JsonValue* code = error == nullptr ? nullptr : error->find("code");
    if (code == nullptr) {
        return 0;
    }
    const auto value = code->as_integer();
    return value.has_value() ? *value : 0;
}

} // namespace mcp_test
