// SPDX-License-Identifier: MIT

#include "project_files.hpp"
#include "project_tools.hpp"
#include "regenerate.hpp"
#include "tessera/harness/json_write.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace tessera::mcp {
namespace {

namespace cli = tessera::compiler::codegen::cli;

ToolResult failure(const policy::ProjectFileError& error) {
    return ToolResult{true, error.code + ": " + error.message};
}

// The resolved project and its listing, or the error result to return.
struct Loaded {
    const OpenProject* project = nullptr;
    ProjectListing listing;
};

std::expected<Loaded, ToolResult> load(SessionState& state) {
    const OpenProject* project = state.resolved_project();
    if (project == nullptr) {
        return std::unexpected(ToolResult{
            true, "TESSERA_SEC_PATH_ESCAPE: the project root cannot be read; open a project "
                  "with `open_project` or start the server in the project folder"});
    }
    auto listing = list_project(project->directory);
    if (!listing) {
        return std::unexpected(failure(listing.error()));
    }
    return Loaded{project, std::move(*listing)};
}

// The options of a folder-wide compile: every readable source file, the entry first, spelled as the
// project-relative names the diagnostics print, resolved against the root rather than against the
// process's working directory.
cli::Options folder_options(const Loaded& loaded) {
    cli::Options options;
    std::vector<std::string> sources;
    for (const ListedFile& file : loaded.listing.files) {
        if (file.kind == policy::ProjectFileKind::Source) {
            sources.push_back(file.path);
        }
    }
    const auto entry = std::find(sources.begin(), sources.end(), loaded.listing.entry);
    if (entry != sources.end() && entry != sources.begin()) {
        std::rotate(sources.begin(), entry, entry + 1);
    }
    options.inputs = std::move(sources);
    options.root = loaded.project->directory.string();
    options.base_dir = loaded.project->directory.string();
    options.query = true;
    return options;
}

} // namespace

ToolResult run_list_files_tool(SessionState& state) {
    auto loaded = load(state);
    if (!loaded) {
        return loaded.error();
    }
    JsonValue files = JsonValue::array();
    for (const ListedFile& file : loaded->listing.files) {
        JsonValue entry = JsonValue::object();
        entry.set("path", JsonValue::string(file.path));
        entry.set("kind", JsonValue::string(std::string(policy::to_string(file.kind))));
        entry.set("size", JsonValue::integer(static_cast<std::int64_t>(file.size)));
        entry.set("readable", JsonValue::boolean(policy::is_readable(file.kind)));
        if (!file.digest.empty()) {
            entry.set("digest", JsonValue::string(file.digest));
        }
        files.push_back(std::move(entry));
    }
    JsonValue document = JsonValue::object();
    document.set("folder", JsonValue::string(loaded->project->folder));
    document.set("rootSource", JsonValue::string(std::string(to_string(loaded->project->source))));
    document.set("files", std::move(files));
    auto text = tessera::harness::to_json(document);
    if (!text) {
        return ToolResult{true, "TESSERA_MCP_INVALID_PARAMS: the listing cannot be encoded"};
    }
    return ToolResult{false, std::move(*text)};
}

ToolResult run_project_diagnostics_tool(SessionState& state) {
    auto loaded = load(state);
    if (!loaded) {
        return loaded.error();
    }
    const cli::Options options = folder_options(*loaded);
    const cli::Outcome outcome = cli::run(options);
    return ToolResult{false, cli::diagnostics_document(options, outcome)};
}

ToolResult run_project_ir_tool(SessionState& state) {
    auto loaded = load(state);
    if (!loaded) {
        return loaded.error();
    }
    cli::Options options = folder_options(*loaded);
    options.capture_ir = true;
    const cli::Outcome outcome = cli::run(options);
    if (outcome.exit_code != 0 || outcome.ir_json.empty()) {
        return ToolResult{true, cli::diagnostics_document(options, outcome)};
    }
    return ToolResult{false, outcome.ir_json};
}

} // namespace tessera::mcp
