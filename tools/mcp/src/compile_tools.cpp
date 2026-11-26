// SPDX-License-Identifier: MIT

#include "compile_tools.hpp"
#include "diagnose.hpp"
#include "in_memory_ir.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::mcp {
namespace {

namespace codegen_cli = tessera::compiler::codegen::cli;

// The diagnostics document a failed `get_ir` returns. `diagnose_source()` is the one producer, so
// the text a client sees is the document the `diagnostics` tool would have returned for the same
// input, not a re-worded message from this layer.
ToolResult diagnostics_error(std::string_view source, std::string_view file,
                             std::string_view services_manifest) {
    return ToolResult{true, codegen_cli::diagnose_source(source, file, services_manifest)};
}

} // namespace

std::string read_project_services_manifest(SessionState& session) {
    const OpenProject* const project = session.resolved_project();
    if (project == nullptr) {
        return {};
    }
    std::ifstream in(project->directory / "ui" / "services.json", std::ios::binary);
    if (!in) {
        return {}; // a project with no manifest has no services
    }
    std::string text(kMaxSourceBytes + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (text.size() > kMaxSourceBytes) {
        text.clear(); // over the per-file limit: treated as absent, the front end then rejects uses
    }
    return text;
}

std::string resolve_buffer_name(const SessionState& session, std::string_view file) {
    (void)session; // the project root plays no part until a tool reads files (D-S7)
    if (file.empty()) {
        return std::string(kDefaultBufferName);
    }
    return normalize_project_path(file);
}

ToolResult run_diagnostics_tool(std::string_view source, std::string_view file,
                                std::string_view services_manifest) {
    // No transformation, no truncation, no second limit: the front end's own caps already bound the
    // work and the text (docs/specs/compiler-diagnostics-v1.md section 4).
    return ToolResult{false, codegen_cli::diagnose_source(source, file, services_manifest)};
}

ToolResult run_get_ir_tool(std::string_view source, std::string_view file,
                           std::string_view services_manifest) {
    std::optional<std::string> ir_text = lower_source_to_ir_json(source, file, services_manifest);
    if (!ir_text) {
        return diagnostics_error(source, file, services_manifest);
    }
    return ToolResult{false, std::move(*ir_text)};
}

} // namespace tessera::mcp
