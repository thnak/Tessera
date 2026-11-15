// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/json_value.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The tool surface of `tessera-mcp` (docs/specs/designer-studio-v1.md section 8, slice D-S6 of the
// same document, decisions/ADR-045 item 6).
//
// The table here is the single declaration of the surface: `tools/list` is built from it and
// `tools/call` looks a name up in it, so a tool cannot be advertised without being dispatchable or
// dispatched without being advertised. A **declared** tool is one this build cannot perform yet;
// calling it returns an MCP execution error naming that, which is the honest answer and the one a
// client can branch on (`TESSERA_MCP_TOOL_UNIMPLEMENTED`). Nothing here is a stub that pretends.
namespace tessera::mcp {

using tessera::harness::JsonValue;

// What this build can do with a tool. The distinction is published, not implied: a declared tool's
// description says so, so an agent reading `tools/list` never has to discover it by calling.
enum class ToolStatus {
    Implemented,
    Declared, // advertised, returns TESSERA_MCP_TOOL_UNIMPLEMENTED
};

// Where the tool's work comes from once it is implemented. This records the mapping the spec
// requires ("every tool maps to one command of headless-automation-v1.md or one message of the
// designer protocol"), and it is what a later slice fills in rather than inventing a new protocol.
enum class ToolSource {
    Compiler,         // the front end / codegen driver, as the CLI harness runs it
    Harness,          // a headless-automation-v1 command (needs a live host)
    DesignerProtocol, // a designer-protocol-v1 host message (needs the studio host)
};

struct ToolDefinition {
    std::string_view name;
    std::string_view description;
    ToolStatus status = ToolStatus::Declared;
    ToolSource source = ToolSource::Harness;
    JsonValue input_schema{JsonValue::object()};
};

// Every tool of the surface, in the spec's order. Stable across calls: the same build answers the
// same `tools/list` bytes, and a client may cache by name.
[[nodiscard]] const std::vector<ToolDefinition>& tools();

// The definition for `name`, or nullptr. A lookup, not a search: the tool list is small and fixed.
[[nodiscard]] const ToolDefinition* find_tool(std::string_view name) noexcept;

// The `tools/list` result object: one entry per tool, in declaration order, with name, description,
// inputSchema, and an `annotations.readOnlyHint`/`status` extension field the client may ignore.
[[nodiscard]] JsonValue tools_list_result();

// A single MCP tool call result: the content blocks plus `isError`, which is the channel MCP
// reserves for "the tool ran and failed" as opposed to a protocol error.
struct ToolResult {
    bool is_error = false;
    std::string text; // the single text content block

    friend bool operator==(const ToolResult&, const ToolResult&) = default;
};

// Builds the MCP `result` object of a tool call from a ToolResult.
[[nodiscard]] JsonValue tool_result_json(const ToolResult& result);

// The standard "not implemented in this build" result for a declared tool. The text names the
// owning slice and the mapped source, so an agent can tell a missing feature from a broken one.
[[nodiscard]] ToolResult unimplemented_tool_result(const ToolDefinition& tool);

} // namespace tessera::mcp
