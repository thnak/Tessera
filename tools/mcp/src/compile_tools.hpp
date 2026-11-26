// SPDX-License-Identifier: MIT

#pragma once

#include "tool_registry.hpp"
#include "tool_session.hpp"

#include <string_view>

// The compile-only tools: the part of the D-S6 surface that the front end can already answer.
// Both run the *same* pipeline the diagnostics writer owns (`tools/codegen/diagnose.cpp` and the
// codegen IR emitter), so the adapter cannot drift from the CLI: `diagnostics` returns the
// document `tessera_codegen --diagnostics-json` writes, byte for byte, which is the issue's
// acceptance.
namespace tessera::mcp {

// `diagnostics`: compile `source` as `file` and return the
// docs/specs/compiler-diagnostics-v1.md document as the tool's text content.
[[nodiscard]] ToolResult run_diagnostics_tool(std::string_view source, std::string_view file,
                                              std::string_view services_manifest = {});

// `get_ir`: lower `source` and return the canonical IR JSON (docs/specs/ir-v1.md). When the source
// does not reach a valid module, the diagnostics document is returned with `isError` set, because a
// client asking for IR needs to know why there is none.
[[nodiscard]] ToolResult run_get_ir_tool(std::string_view source, std::string_view file,
                                         std::string_view services_manifest = {});

// The text of the open project's `ui/services.json` (docs/specs/services-manifest-v1.md), or empty
// when the project has none, so both compile tools resolve `useService<T>()` the way the CLI does.
[[nodiscard]] std::string read_project_services_manifest(SessionState& session);

// Resolves the `file` argument of a compile tool against the open project, defaulting to
// `kDefaultBufferName`. An empty result means the path escapes the project and must be refused.
inline constexpr std::string_view kDefaultBufferName = "buffer.tsx";

[[nodiscard]] std::string resolve_buffer_name(const SessionState& session, std::string_view file);

} // namespace tessera::mcp
