// SPDX-License-Identifier: MIT

#include "compile_tools.hpp"
#include "diagnose.hpp"
#include "in_memory_ir.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::mcp {
namespace {

namespace codegen_cli = tessera::compiler::codegen::cli;

// The diagnostics document a failed `get_ir` returns. `diagnose_source()` is the one producer, so
// the text a client sees is the document the `diagnostics` tool would have returned for the same
// input, not a re-worded message from this layer.
ToolResult diagnostics_error(std::string_view source, std::string_view file) {
    return ToolResult{true, codegen_cli::diagnose_source(source, file)};
}

} // namespace

std::string resolve_buffer_name(const SessionState& session, std::string_view file) {
    (void)session; // the project root plays no part until a tool reads files (D-S7)
    if (file.empty()) {
        return std::string(kDefaultBufferName);
    }
    return normalize_project_path(file);
}

ToolResult run_diagnostics_tool(std::string_view source, std::string_view file) {
    // No transformation, no truncation, no second limit: the front end's own caps already bound the
    // work and the text (docs/specs/compiler-diagnostics-v1.md section 4).
    return ToolResult{false, codegen_cli::diagnose_source(source, file)};
}

ToolResult run_get_ir_tool(std::string_view source, std::string_view file) {
    std::optional<std::string> ir_text = lower_source_to_ir_json(source, file);
    if (!ir_text) {
        return diagnostics_error(source, file);
    }
    return ToolResult{false, std::move(*ir_text)};
}

} // namespace tessera::mcp
