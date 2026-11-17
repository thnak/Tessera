// SPDX-License-Identifier: MIT

#pragma once

#include "tool_registry.hpp"
#include "tool_session.hpp"

#include <string_view>

// The tools that read the open project folder (docs/specs/designer-studio-v1.md section 8):
// `list_files`, and `diagnostics` / `get_ir` when the call carries no `source`. They are read-only:
// the server writes nothing, runs no shell and never reads outside the root. The project is the
// session's resolved root (an explicit `open_project`, else the client's `roots`, else the working
// directory), and every file it touches is judged by the shared policy of
// `tessera/schema/project_files.hpp`, the one the Wasm module links.
namespace tessera::mcp {

// The folder's files: project-relative POSIX path, kind, size, readability and (for a readable
// file) a content digest. Names the root only by its folder name and `rootSource`.
[[nodiscard]] ToolResult run_list_files_tool(SessionState& state);

// The folder-wide diagnostics document: the same driver and the same serializer as
// `tessera_codegen --diagnostics-json` over the project's source files, entry first.
[[nodiscard]] ToolResult run_project_diagnostics_tool(SessionState& state);

// The folder's canonical IR (ir-v1.md), or the diagnostics document with `isError` when the
// project does not reach one.
[[nodiscard]] ToolResult run_project_ir_tool(SessionState& state);

// `generate` (ADR-040, issue #407): the C++ of the folder, by the same driver as `tessera_codegen`.
// Without `write` it is a dry run: it reports the generated files (size, SHA-256, new / changed /
// unchanged) and writes nothing. With `write` it regenerates the generated bases inside the output
// directory and scaffolds developer classes that do not exist; it never modifies an existing
// developer file. `path` returns one generated file's text instead of the summary.
[[nodiscard]] ToolResult run_generate_tool(SessionState& state, bool write, std::string_view path);

} // namespace tessera::mcp
