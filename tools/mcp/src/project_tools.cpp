// SPDX-License-Identifier: MIT

#include "project_files.hpp"
#include "project_tools.hpp"
#include "regenerate.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/ui/automation/sha256.hpp"

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

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// Removes the host's absolute project path from text the driver wrote (a setup error, a warning
// that names a developer file): the server never sends an absolute path to the client. The path
// appears in its generic, native and JSON-escaped native spellings.
std::string redact(std::string text, const std::filesystem::path& directory) {
    const std::string generic = directory.generic_string();
    const std::string native = directory.string();
    std::string escaped;
    for (const char c : native) {
        escaped += c;
        if (c == '\\') {
            escaped += c;
        }
    }
    for (const std::string& form : {escaped, native, generic}) {
        replace_all(text, form + "\\\\", "");
        replace_all(text, form + "\\", "");
        replace_all(text, form + "/", "");
        replace_all(text, form, ".");
    }
    return text;
}

// True when a symbolic link at `link` lies on the way to `out_dir`, or inside it.
bool link_touches(std::string_view link, std::string_view out_dir) {
    const auto prefix_of = [](std::string_view shorter, std::string_view longer) {
        return longer == shorter ||
               (longer.size() > shorter.size() && longer.compare(0, shorter.size(), shorter) == 0 &&
                longer[shorter.size()] == '/');
    };
    return prefix_of(link, out_dir) || prefix_of(out_dir, link);
}

JsonValue string_array(const std::vector<std::string>& values) {
    JsonValue array = JsonValue::array();
    for (const std::string& value : values) {
        array.push_back(JsonValue::string(value));
    }
    return array;
}

std::string_view status_name(cli::GeneratedStatus status) {
    switch (status) {
    case cli::GeneratedStatus::New:
        return "new";
    case cli::GeneratedStatus::Changed:
        return "changed";
    case cli::GeneratedStatus::Unchanged:
        return "unchanged";
    }
    return "unchanged";
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

namespace tessera::mcp {

ToolResult run_generate_tool(SessionState& state, bool write, std::string_view path) {
    auto loaded = load(state);
    if (!loaded) {
        return loaded.error();
    }
    const std::string out_dir = loaded->listing.out_dir;
    // The output directory is a project path like any other, and one tool never writes into a
    // directory the walk does not enter or through a link it never follows.
    if (auto bad = policy::check_project_path(out_dir)) {
        return failure(*bad);
    }
    const std::string first = out_dir.substr(0, out_dir.find('/'));
    if (policy::is_pruned_directory(first)) {
        return ToolResult{true, "TESSERA_SEC_PATH_ESCAPE: the output directory `" + out_dir +
                                    "` is inside a directory the server does not enter"};
    }
    for (const ListedFile& file : loaded->listing.files) {
        if (file.link && link_touches(file.path, out_dir)) {
            return ToolResult{true,
                              "TESSERA_SEC_PATH_ESCAPE: `" + file.path +
                                  "` is a symbolic link on the path of the output directory `" +
                                  out_dir + "`"};
        }
    }

    // Reading one file never writes: `path` makes the call a dry run whatever `write` says.
    const bool dry = !write || !path.empty();
    cli::Options options = folder_options(*loaded);
    options.query = false;
    options.output_dir = (loaded->project->directory / std::filesystem::path(out_dir)).string();
    options.dry_run = dry;
    options.collect_generated = true;
    const cli::Outcome outcome = cli::run(options);
    if (outcome.exit_code != 0) {
        // A blocking diagnostic: nothing was written (every file is computed before the first
        // write), and the client gets the document that says why.
        return ToolResult{
            true, redact(cli::diagnostics_document(options, outcome), loaded->project->directory)};
    }

    const std::string prefix = out_dir + "/";
    if (!path.empty()) {
        for (const cli::GeneratedEntry& entry : outcome.generated) {
            if (path == entry.path || path == prefix + entry.path) {
                return ToolResult{false, entry.text};
            }
        }
        std::string names;
        for (const cli::GeneratedEntry& entry : outcome.generated) {
            names += (names.empty() ? "" : ", ") + prefix + entry.path;
        }
        return ToolResult{true,
                          "TESSERA_MCP_INVALID_PARAMS: `" + std::string(path) +
                              "` is not a generated file of this project; the generated files "
                              "are " +
                              names};
    }

    JsonValue files = JsonValue::array();
    std::vector<std::string> written;
    bool drift = false;
    for (const cli::GeneratedEntry& entry : outcome.generated) {
        JsonValue item = JsonValue::object();
        item.set("path", JsonValue::string(prefix + entry.path));
        item.set("size", JsonValue::integer(static_cast<std::int64_t>(entry.text.size())));
        item.set("digest",
                 JsonValue::string("sha256:" + tessera::ui::automation::to_hex(
                                                   tessera::ui::automation::sha256(entry.text))));
        item.set("status", JsonValue::string(std::string(status_name(entry.status))));
        files.push_back(std::move(item));
        if (entry.status != cli::GeneratedStatus::Unchanged) {
            drift = true;
            written.push_back(prefix + entry.path);
        }
    }
    std::vector<std::string> scaffold;
    for (const std::string& stub : outcome.scaffolded) {
        scaffold.push_back(prefix + stub);
    }
    std::vector<std::string> warnings;
    for (const cli::Message& message : outcome.messages) {
        warnings.push_back(redact(message.text, loaded->project->directory));
    }

    JsonValue document = JsonValue::object();
    document.set("write", JsonValue::boolean(!dry));
    document.set("outDir", JsonValue::string(out_dir));
    // `check` semantics of the CLI: 1 when a dry run finds a file that is new or differs.
    document.set("exitCode", JsonValue::integer(dry && drift ? 1 : 0));
    document.set("drift", JsonValue::boolean(drift));
    document.set("files", std::move(files));
    document.set("written", string_array(dry ? std::vector<std::string>{} : written));
    document.set("scaffold", string_array(scaffold));
    document.set("warnings", string_array(warnings));
    auto text = tessera::harness::to_json(document);
    if (!text) {
        return ToolResult{true, "TESSERA_MCP_INVALID_PARAMS: the summary cannot be encoded"};
    }
    return ToolResult{false, std::move(*text)};
}

} // namespace tessera::mcp
