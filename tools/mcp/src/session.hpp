// SPDX-License-Identifier: MIT

#pragma once

#include "jsonrpc.hpp"
#include "tool_session.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The MCP lifecycle and the `tools/*` methods, over the JSON-RPC codec. One call in, zero or one
// line out: a request is always answered, a notification never is (JSON-RPC forbids a response to
// one). The session owns no thread and no timer, so a client's ordering is the only ordering.
namespace tessera::mcp {

// What one line produced. `reply` is empty for a notification that needs no answer, and for a
// request whose reply could not be encoded (which the serve loop reports and ends on: a server that
// cannot answer is no longer usable).
struct SessionReply {
    std::string reply;
    bool answered = false; // a request was answered (true) rather than a notification handled
    // Lines the server sends on its own after handling this message: today only the `roots/list`
    // request, issued once the client has acknowledged the handshake (or told us its roots
    // changed). The serve loop writes them after `reply`, in order.
    std::vector<std::string> outbound;
};

// The methods this server implements, as the `initialize` handshake requires. Anything else is
// MethodNotFound, which is the correct answer for a capability that was not advertised.
class McpSession {
public:
    McpSession() = default;
    McpSession(const McpSession&) = delete;
    McpSession& operator=(const McpSession&) = delete;

    // Handles one decoded line. `line` is the raw text, kept for the parse-error path.
    [[nodiscard]] std::expected<SessionReply, std::string_view> handle_line(std::string_view line);

    // Handles one decoded message. Split from handle_line so a test can drive messages directly and
    // so the codec's failure path has exactly one owner in handle_line.
    [[nodiscard]] std::expected<SessionReply, std::string_view>
    handle_message(const IncomingMessage& message);

    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    [[nodiscard]] const SessionState& state() const noexcept {
        return state_;
    }

private:
    [[nodiscard]] std::expected<JsonValue, RpcError> initialize(const JsonValue& params);
    [[nodiscard]] std::expected<JsonValue, RpcError> call_tool(const JsonValue& params);
    // The next `roots/list` request line, with an id this session recognises in the answer.
    [[nodiscard]] std::string next_roots_request();
    // The client's answer to that request: the first usable root becomes the project.
    void handle_roots_response(const IncomingMessage& message);

    SessionState state_;
    // A request before `initialize` is refused with NotInitialized (the MCP lifecycle section),
    // except for `ping`, which exists to probe liveness and is harmless before the handshake.
    bool initialized_ = false;
    // The id of the `roots/list` request in flight, empty when none. A response with any other id
    // is not ours and is dropped.
    std::string pending_roots_id_;
    std::uint64_t roots_requests_ = 0;
};

// The `initialize` result: negotiated protocol version, the tools capability, and server identity.
[[nodiscard]] JsonValue initialize_result(std::string_view requested_version);

} // namespace tessera::mcp
