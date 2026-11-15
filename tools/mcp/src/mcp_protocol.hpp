// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>

// Constants of the Model Context Protocol binding this adapter speaks: JSON-RPC 2.0 messages, one
// per line, over the process's stdin/stdout (docs/specs/designer-studio-v1.md section 8,
// decisions/ADR-045 item 6). Everything here is wire surface, so it is named once and every other
// unit of the tool refers to these names instead of repeating a literal.
namespace tessera::mcp {

// The JSON-RPC revision on every envelope. MCP requires exactly this string.
inline constexpr std::string_view kJsonRpcVersion = "2.0";

// Server identity reported by `initialize`. The version is the project version injected by CMake
// (TESSERA_MCP_VERSION); the fallback keeps the header usable by a unit test that does not have it.
inline constexpr std::string_view kServerName = "tessera-mcp";
#ifdef TESSERA_MCP_VERSION
inline constexpr std::string_view kServerVersion = TESSERA_MCP_VERSION;
#else
inline constexpr std::string_view kServerVersion = "0.0.0";
#endif

// Newest protocol revision this server implements. `initialize` answers with the client's version
// when it is one of the supported revisions, and with this one otherwise: the client then decides
// whether it can continue, which is the negotiation the specification describes.
inline constexpr std::string_view kLatestProtocolVersion = "2025-06-18";

// The revisions whose lifecycle and `tools/*` shapes this server implements. They differ only in
// features the adapter does not use (batching, resource subscriptions), so one implementation
// serves all three; a revision outside the set is answered with kLatestProtocolVersion.
inline constexpr std::string_view kSupportedProtocolVersions[] = {"2025-06-18", "2025-03-26",
                                                                  "2024-11-05"};

[[nodiscard]] bool protocol_version_supported(std::string_view version) noexcept;

// JSON-RPC 2.0 error codes, plus the two MCP-specific ones this server can produce. The numeric
// values are the specification's; -32002 comes from the MCP lifecycle section.
enum class RpcErrorCode : std::int64_t {
    ParseError = -32700,     // the line is not JSON
    InvalidRequest = -32600, // not a JSON-RPC request/notification object
    MethodNotFound = -32601, // no such method
    InvalidParams = -32602,  // params do not match the method's schema
    InternalError = -32603,  // the server failed
    NotInitialized = -32002, // a request arrived before `initialize`
};

// Stable `TESSERA_*` codes carried in `error.data.code`, so a client can branch without matching
// prose (docs/specs/error-model-v1.md section 3). The prefix table has no MCP family yet; these
// names are declared here and promoted to the error model when the tool surface gets a spec.
inline constexpr std::string_view kUnknownToolCode = "TESSERA_MCP_UNKNOWN_TOOL";
inline constexpr std::string_view kUnimplementedToolCode = "TESSERA_MCP_TOOL_UNIMPLEMENTED";
inline constexpr std::string_view kNotInitializedCode = "TESSERA_MCP_NOT_INITIALIZED";
inline constexpr std::string_view kInvalidParamsCode = "TESSERA_MCP_INVALID_PARAMS";
inline constexpr std::string_view kInputTooLargeCode = "TESSERA_MCP_INPUT_TOO_LARGE";

// A tool whose declared arguments are structurally wrong is a protocol error (the request is
// invalid); a tool that accepted its arguments and then failed reports the failure inside a normal
// result with `isError: true`, which is the channel MCP reserves for that case.
struct RpcError {
    RpcErrorCode code = RpcErrorCode::InternalError;
    std::string_view message;
    std::string_view data_code; // TESSERA_* code written to error.data.code; empty omits `data`
};

} // namespace tessera::mcp
