// SPDX-License-Identifier: MIT

// Codegen CLI driver: read .tsx -> parse/analyse -> lower -> validate IR -> generate C++ -> rewrite
// what changed (docs/guides/developer-workflow.md). There is one codegen path (ADR-040): the
// generated `<Component>.g.hpp` base is rewritten wholesale and the developer
// `<Component>.hpp/.cpp` files are scaffolded once and never edited.

#include "diagnostic_record.hpp"
#include "emit_library_components.hpp"
#include "package_manifest.hpp"
#include "project_identity.hpp"
#include "regenerate.hpp"
#include "regenerate_assets.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/harness/json_parse.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::cli {

using internal::build_stylesheet_embeds;
using internal::build_theme_embed;
using internal::import_target;
using internal::PackageRootSpec;
using internal::ProjectIdentity;
using internal::read_file;
using internal::report;
using internal::resolve_identity;
using internal::ThemePinSpec;

namespace {

namespace fs = std::filesystem;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

struct SourceFile {
    SourceFile(std::string file_path, std::string file_text)
        : path(std::move(file_path)),
          text(std::move(file_text)),
          parsed(jsx::parse_module(text)),
          map(text),
          analyzed(parsed.root != nullptr ? rules::analyze_module(*parsed.root, map)
                                          : rules::AnalyzedModule{}) {}

    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;

    [[nodiscard]] bool parses() const {
        return parsed.root != nullptr && !parsed.has_errors();
    }

    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{path, parsed.root.get(), &analyzed, &map};
    }

    std::string path;
    std::string text;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;
};

std::string diagnostic_text(std::string_view path, const jsx::Diagnostic& diagnostic) {
    return std::string(path) + ":" + jsx::format_diagnostic(diagnostic);
}

// Reports a front-end, rules or lowering diagnostic: the printed text is unchanged, and the record
// carries the code and span the pipeline already had (docs/specs/compiler-diagnostics-v1.md).
void report_diagnostic(Outcome& outcome, std::string_view path, const jsx::Diagnostic& diagnostic) {
    report(outcome, diagnostic.severity == jsx::Severity::Error, diagnostic_text(path, diagnostic),
           to_record(diagnostic, path));
}

std::string ir_diagnostic_text(const ir::IrDiagnostic& diagnostic) {
    const ir::SourceSpan& span = diagnostic.span;
    std::string text;
    if (!span.file.empty()) {
        text +=
            span.file + ":" + std::to_string(span.line) + ":" + std::to_string(span.column) + ": ";
    }
    return text + diagnostic.code + ": " + diagnostic.message;
}

bool safe_file_name(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find("..") == std::string_view::npos;
}

// A module namespace when --namespace is absent: the file stem with non-identifier characters
// replaced, leading digits prefixed, and lowercased (C++ namespace convention).
std::string derive_module_name(const std::string& input) {
    const fs::path path(input);
    std::string stem = path.stem().string();
    std::string name;
    for (const char c : stem) {
        const auto ch = static_cast<unsigned char>(c);
        name += ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                 c == '_')
                    ? static_cast<char>(ch >= 'A' && ch <= 'Z' ? c - 'A' + 'a' : c)
                    : '_';
    }
    if (name.empty()) {
        name = "module";
    }
    if (name.front() >= '0' && name.front() <= '9') {
        name.insert(name.begin(), '_');
    }
    return name;
}

bool generate_files(const ir::IrModule& module, std::span<const StylesheetEmbed> embeds,
                    const std::optional<EmbeddedTheme>& theme, const Options& options,
                    std::vector<GeneratedFile>& files, Outcome& outcome, std::string_view input) {
    const auto generated = generate(module, embeds, theme, CodegenOptions{options.line_directives});
    if (!generated) {
        const CodegenErrors& errors = generated.error();
        report(outcome, true,
               std::string(input) + ": " +
                   (errors.empty() ? std::string("codegen failed") : errors.front()));
        outcome.exit_code = 1;
        return false;
    }
    files = *generated;
    for (const std::string& warning : codegen_warnings(module, embeds)) {
        report(outcome, false, std::string(input) + ": warning: " + warning);
    }
    return true;
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// True when `text` contains `member(` as a whole word, i.e. a declaration, definition or call.
bool defines_member(std::string_view text, std::string_view member) {
    const std::string needle = std::string(member) + "(";
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + 1)) {
        if (at == 0 || !is_word_char(text[at - 1])) {
            return true;
        }
    }
    return false;
}

// ADR-040 decision item 6: the developer files are written once, never edited afterwards. An
// existing developer file is only checked: a binding it does not define yet is reported with the
// stub to add, and a stub marker (`// bind::<id>`) whose binding left the TSX is reported unused.
void handle_developer_files(const ir::IrModule& module, const fs::path& output_dir, bool check_only,
                            bool verbose, Outcome& outcome) {
    const auto stubs = scaffold(module);
    if (!stubs) {
        report(outcome, true, "tessera_codegen: " + stubs.error().front());
        outcome.exit_code = 1;
        return;
    }
    std::string error;
    const std::vector<RequiredBinding> required = required_bindings(module, error);
    std::set<std::string> components;
    for (const GeneratedFile& stub : *stubs) {
        components.insert(fs::path(stub.path).stem().string());
    }
    for (const std::string& component : components) {
        const fs::path header = output_dir / (component + ".hpp");
        const fs::path source = output_dir / (component + ".cpp");
        const bool exists = fs::exists(header) || fs::exists(source);
        if (!exists) {
            if (check_only) {
                continue; // developer files are not generated content: nothing to drift from
            }
            for (const GeneratedFile& stub : *stubs) {
                if (fs::path(stub.path).stem().string() != component) {
                    continue;
                }
                std::ofstream out(output_dir / stub.path, std::ios::binary | std::ios::trunc);
                out << stub.text;
                if (!out) {
                    report(outcome, true,
                           "tessera_codegen: cannot write `" + (output_dir / stub.path).string() +
                               "`");
                    outcome.exit_code = 2;
                    return;
                }
                if (verbose) {
                    report(outcome, false, (output_dir / stub.path).string() + ": scaffolded");
                }
            }
            continue;
        }
        const std::string text =
            read_file(header).value_or("") + "\n" + read_file(source).value_or("");
        std::set<std::string> ids;
        for (const RequiredBinding& binding : required) {
            if (binding.component != component) {
                continue;
            }
            ids.insert(binding.id);
            if (!defines_member(text, binding.member)) {
                report(outcome, false,
                       header.string() + ": warning: TESSERA_CG_MISSING_BINDING: component `" +
                           component + "` needs `" + binding.signature + "` for " + binding.id +
                           " (" + binding.source + "); add it to the developer class");
            }
        }
        std::size_t line_start = 0;
        while (line_start < text.size()) {
            const std::size_t line_end = text.find('\n', line_start);
            std::string_view line(text.data() + line_start,
                                  (line_end == std::string::npos ? text.size() : line_end) -
                                      line_start);
            line_start = line_end == std::string::npos ? text.size() : line_end + 1;
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.remove_prefix(1);
            }
            constexpr std::string_view kMarker = "// bind::";
            if (line.substr(0, kMarker.size()) != kMarker) {
                continue;
            }
            std::string id(line.substr(3));
            id = id.substr(0, id.find_first_of(" ("));
            if (ids.count(id) == 0) {
                report(outcome, false,
                       header.string() + ": warning: TESSERA_CG_BINDING_UNUSED: `" + id +
                           "` is no longer in the TSX of component `" + component +
                           "`; its developer member is now unused");
            }
        }
    }
}

} // namespace

namespace {

// The file on disk an input names: relative to `base_dir` when the caller gave one, else as the
// command line spelled it (relative to the working directory, or absolute).
fs::path disk_input(const Options& options, const std::string& input) {
    return options.base_dir.empty() ? fs::path(input)
                                    : fs::path(options.base_dir) / fs::path(input);
}

// The body of run(), split out so the `--diagnostics-json` document is written on every exit path
// (success, a diagnostic, a setup failure) without repeating the write at each return.
Outcome run_impl(const Options& options) {
    Outcome outcome;
    if (options.inputs.empty()) {
        report(outcome, true, "tessera_codegen: no input .tsx file");
        outcome.exit_code = 2;
        return outcome;
    }
    const int output_modes =
        (!options.emit_ir_path.empty() ? 1 : 0) + (!options.emit_library_cpp_path.empty() ? 1 : 0) +
        (!options.diagnostics_json_path.empty() ? 1 : 0) + (options.query ? 1 : 0);
    if (output_modes > 1) {
        report(outcome, true,
               "tessera_codegen: choose one of --emit-ir, --emit-library-cpp and "
               "--diagnostics-json");
        outcome.exit_code = 2;
        return outcome;
    }
    const bool emits = !options.emit_ir_path.empty() || !options.emit_library_cpp_path.empty();
    // `--diagnostics-json` needs no --out: it is another "write one file, generate no C++" mode.
    const bool diagnostics_only = !options.diagnostics_json_path.empty() || options.query;
    if (!emits && !diagnostics_only && options.output_dir.empty()) {
        report(outcome, true, "tessera_codegen: --out is required");
        outcome.exit_code = 2;
        return outcome;
    }

    const std::optional<ProjectIdentity> identity = resolve_identity(options, outcome);
    if (!identity) {
        return outcome;
    }
    if (!check_package_versions(*identity, outcome)) {
        return outcome;
    }
    std::optional<EmbeddedTheme> theme;
    if (!build_theme_embed(*identity, outcome, theme)) {
        return outcome;
    }

    // Project-relative names are required with a salt (they feed the ids) and with package roots
    // (bare specifiers match against them); either way the form is POSIX and checkout-independent.
    const bool project_paths = !identity->salt.empty() || !identity->packages.empty();
    std::vector<std::unique_ptr<SourceFile>> sources;
    std::vector<std::pair<std::string, fs::path>> pending; // (project path, file on disk)
    std::set<std::string> seen;
    for (const std::string& input : options.inputs) {
        std::string path = input;
        if (project_paths) {
            const fs::path& root = identity->root;
            std::error_code relative_error;
            const fs::path relative =
                fs::relative(fs::absolute(disk_input(options, input)), root, relative_error);
            if (relative_error || relative.empty() || *relative.begin() == "..") {
                report(outcome, true,
                       "tessera_codegen: `" + input + "` is outside the project root `" +
                           root.generic_string() + "`");
                outcome.exit_code = 2;
                return outcome;
            }
            path = relative.generic_string();
        }
        if (seen.insert(path).second) {
            pending.emplace_back(std::move(path), disk_input(options, input));
        }
    }
    while (!pending.empty()) {
        std::pair<std::string, fs::path> next = std::move(pending.front());
        pending.erase(pending.begin());
        std::optional<std::string> text = read_file(next.second);
        if (!text) {
            report(outcome, true,
                   "tessera_codegen: cannot read `" + next.second.generic_string() + "`");
            outcome.exit_code = 2;
            return outcome;
        }
        sources.push_back(std::make_unique<SourceFile>(std::move(next.first), std::move(*text)));
        // Import closure (issue #70): a run follows the imports of every file it loads, so a
        // package's sources do not have to be listed on the command line. A specifier that names no
        // file is left alone here - lowering reports it with the import diagnostics.
        const SourceFile& source = *sources.back();
        if (!project_paths || !source.parses()) {
            continue;
        }
        for (const jsx::ModuleItem& item : source.parsed.root->items) {
            if (item.kind != jsx::ItemKind::Import || item.side_effect) {
                continue;
            }
            const std::optional<std::string> target =
                import_target(source.path, item.source, *identity);
            if (!target || !seen.insert(*target).second) {
                continue;
            }
            const fs::path target_disk = identity->root / fs::path(*target);
            std::error_code file_error;
            if (!fs::is_regular_file(target_disk, file_error)) {
                seen.erase(*target);
                continue;
            }
            pending.emplace_back(*target, target_disk);
        }
    }
    for (const auto& source : sources) {
        if (!source->parses()) {
            // A diagnostic wins: a limit check fails without a root, so testing `root == nullptr`
            // first discarded its classified code.
            if (!source->parsed.diagnostics.empty()) {
                report_diagnostic(outcome, source->path, source->parsed.diagnostics.front());
            } else {
                report(outcome, true, source->path + ": the file cannot be parsed");
            }
            outcome.exit_code = 1;
            return outcome;
        }
        if (source->analyzed.has_errors()) {
            if (!source->analyzed.diagnostics.empty()) {
                report_diagnostic(outcome, source->path, source->analyzed.diagnostics.front());
            } else {
                report(outcome, true, source->path + ": analysis failed");
            }
            outcome.exit_code = 1;
            return outcome;
        }
    }

    std::vector<lowering::ModuleSource> module_sources;
    module_sources.reserve(sources.size());
    for (const auto& source : sources) {
        module_sources.push_back(source->module_source());
    }
    const std::string module_name = options.module_name.empty()
                                        ? derive_module_name(options.inputs.front())
                                        : options.module_name;
    // The package-root strings live in `identity`, so these views stay valid for the lowering call.
    std::vector<lowering::PackageRoot> packages;
    packages.reserve(identity->packages.size());
    for (const PackageRootSpec& spec : identity->packages) {
        packages.push_back(lowering::PackageRoot{spec.name, spec.root});
    }
    // Stylesheets of side-effect CSS imports (issue #80): read here, hashed by lowering. A file
    // that cannot be read is simply not offered, so lowering reports it at the import's span.
    std::vector<std::string> stylesheet_paths;
    std::vector<std::string> stylesheet_texts;
    for (const auto& source : sources) {
        for (const jsx::ModuleItem& item : source->parsed.root->items) {
            if (item.kind != jsx::ItemKind::Import || !item.side_effect) {
                continue;
            }
            const std::size_t slash = source->path.rfind('/');
            std::string path = lowering::join_project_path(
                slash == std::string::npos ? std::string_view()
                                           : std::string_view(source->path).substr(0, slash),
                item.source);
            if (std::find(stylesheet_paths.begin(), stylesheet_paths.end(), path) !=
                stylesheet_paths.end()) {
                continue;
            }
            const fs::path on_disk =
                project_paths
                    ? identity->root / fs::path(path)
                    : (disk_input(options, source->path).parent_path() / fs::path(item.source))
                          .lexically_normal();
            std::optional<std::string> text = read_file(on_disk);
            if (!text) {
                // A missing file stays lowering's error at the import's span (TESSERA_IR_
                // STYLESHEET_MISSING); an existing file the CLI cannot read is a build failure.
                std::error_code exists_error;
                if (fs::exists(on_disk, exists_error)) {
                    report(outcome, true,
                           path + ":1:1: TESSERA_CSS_READ: cannot read the stylesheet `" + path +
                               "`");
                    outcome.exit_code = 1;
                    return outcome;
                }
                continue;
            }
            stylesheet_paths.push_back(std::move(path));
            stylesheet_texts.push_back(std::move(*text));
        }
    }
    const std::optional<std::vector<StylesheetEmbed>> embeds =
        build_stylesheet_embeds(stylesheet_paths, stylesheet_texts, outcome);
    if (!embeds) {
        return outcome;
    }
    std::vector<lowering::StylesheetFile> stylesheet_files;
    stylesheet_files.reserve(stylesheet_paths.size());
    for (std::size_t i = 0; i < stylesheet_paths.size(); ++i) {
        stylesheet_files.push_back(
            lowering::StylesheetFile{stylesheet_paths[i], stylesheet_texts[i]});
    }
    lowering::ModuleLoweringResult lowered =
        lowering::lower_project(std::span<const lowering::ModuleSource>(module_sources),
                                module_name, packages, stylesheet_files);
    if (!lowered) {
        const lowering::NodeLoweringErrors& errors = lowered.error();
        if (errors.empty()) {
            report(outcome, true, "lowering failed");
        } else {
            report_diagnostic(outcome, sources.front()->path, errors.front());
        }
        outcome.exit_code = 1;
        return outcome;
    }

    lowering::LoweredModule lowered_module = std::move(*lowered);
    ir::IrModule module;
    module.name = lowered_module.name;
    module.components = std::move(lowered_module.components);
    module.bindings = std::move(lowered_module.bindings);
    module.stylesheets = std::move(lowered_module.stylesheets);
    module.types = std::move(lowered_module.types);

    if (!options.emit_library_cpp_path.empty()) {
        // The build-time library registry has no project: its ids come from the fixed library salt
        // so inserting a library component cannot shift the ids of the others
        // (docs/specs/automation-ids-v1.md section 2.1).
        const auto assigned = tessera::compiler::ids::assign_library_identity(module);
        if (!assigned) {
            report(outcome, true, "tessera_codegen: " + assigned.error().message);
            outcome.exit_code = 1;
            return outcome;
        }
    } else if (!identity->salt.empty()) {
        const auto assigned = tessera::compiler::ids::assign_identity(module, identity->salt);
        if (!assigned) {
            report(outcome, true, "tessera_codegen: " + assigned.error().message);
            outcome.exit_code = 1;
            return outcome;
        }
    }

    // Without a salt nothing computes componentId/uiId/automationId; the validator reports them as
    // missing. They are the only documented diagnostics the CLI filters (docs/guides/
    // developer-workflow.md), and every other IR diagnostic still fails the run.
    const std::vector<ir::IrDiagnostic> all_diagnostics = ir::validate(module);
    const bool identity_assigned =
        !identity->salt.empty() || !options.emit_library_cpp_path.empty();
    const auto deferred_identity = [identity_assigned](const ir::IrDiagnostic& diagnostic) {
        if (identity_assigned || diagnostic.code != "TESSERA_IR_MISSING_ID") {
            return false;
        }
        return diagnostic.message.find("componentId") != std::string::npos ||
               diagnostic.message.find("uiId") != std::string::npos ||
               diagnostic.message.find("automationId") != std::string::npos;
    };
    const auto first_ir = std::find_if(
        all_diagnostics.begin(), all_diagnostics.end(),
        [&](const ir::IrDiagnostic& diagnostic) { return !deferred_identity(diagnostic); });
    if (first_ir != all_diagnostics.end()) {
        report(outcome, true, ir_diagnostic_text(*first_ir), to_record(*first_ir));
        outcome.exit_code = 1;
        return outcome;
    }

    // `--diagnostics-json` is a pure query: it reports what the front end, the analysis, the
    // lowering and the validator found and writes no C++, so a clean input produces an empty
    // `diagnostics` array and exit 0 (docs/specs/compiler-diagnostics-v1.md section 3). No
    // "written" message is reported here: the document holds diagnostics, not progress notes.
    if (diagnostics_only) {
        if (options.capture_ir) {
            outcome.ir_json = ir::write_ir_json(module);
        }
        return outcome;
    }

    // Build-time emission modes. Both write exactly one file and generate no per-component C++;
    // the TSX sources are the single source of truth, so a source that does not lower cleanly
    // stops the run here (the build fails rather than falling back to hand-built data).
    if (emits) {
        const std::string& target_path = options.emit_library_cpp_path.empty()
                                             ? options.emit_ir_path
                                             : options.emit_library_cpp_path;
        const fs::path target(target_path);
        if (target.has_parent_path()) {
            std::error_code directory_error;
            fs::create_directories(target.parent_path(), directory_error);
            if (directory_error) {
                report(outcome, true,
                       "tessera_codegen: cannot create `" + target.parent_path().string() +
                           "`: " + directory_error.message());
                outcome.exit_code = 2;
                return outcome;
            }
        }
        std::string text;
        if (!options.emit_ir_path.empty()) {
            text = ir::write_ir_json(module);
        } else {
            std::string why;
            if (!emit_library_components_cpp(module, target.string(), why)) {
                report(outcome, true, "tessera_codegen: " + why);
                outcome.exit_code = 1;
                return outcome;
            }
            if (options.verbose) {
                report(outcome, false, target.string() + ": written");
            }
            return outcome;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            report(outcome, true, "tessera_codegen: cannot write `" + target.string() + "`");
            outcome.exit_code = 2;
            return outcome;
        }
        if (options.verbose) {
            report(outcome, false, target.string() + ": written");
        }
        return outcome;
    }

    std::vector<GeneratedFile> files;
    if (!generate_files(module, *embeds, theme, options, files, outcome, options.inputs.front())) {
        return outcome;
    }

    if (!identity->salt.empty()) {
        // The manifest resolves the numeric ids to names and sources; it is generated, covered by
        // --check, and has no protected regions.
        files.push_back({"automation.manifest.json",
                         tessera::compiler::ids::write_manifest(module, identity->salt)});
    }

    const fs::path output_dir(options.output_dir);
    std::error_code error;
    fs::create_directories(output_dir, error);
    if (error) {
        report(outcome, true,
               "tessera_codegen: cannot create output directory `" + output_dir.string() +
                   "`: " + error.message());
        outcome.exit_code = 2;
        return outcome;
    }

    bool drift = false;
    for (const GeneratedFile& file : files) {
        if (!safe_file_name(file.path)) {
            report(outcome, true,
                   "tessera_codegen: refusing an unsafe generated path `" + file.path + "`");
            outcome.exit_code = 2;
            return outcome;
        }
        const fs::path target = output_dir / file.path;
        const std::optional<std::string> existing =
            fs::exists(target) ? read_file(target) : std::nullopt;
        std::string content = file.text;
        if (existing) {
            // The generated base and the manifest are rewritten wholesale: they carry no
            // developer-owned content, only the scaffolded `<Component>.hpp/.cpp` does, and those
            // are handled by handle_developer_files() below and never written here.
            if (*existing == content) {
                if (options.verbose) {
                    report(outcome, false, target.string() + ": unchanged");
                }
                continue;
            }
        }
        if (options.check_only) {
            drift = true;
            report(outcome, true, target.string() + ": out of date");
            continue;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            report(outcome, true, "tessera_codegen: cannot write `" + target.string() + "`");
            outcome.exit_code = 2;
            return outcome;
        }
        out << content;
        if (options.verbose) {
            report(outcome, false, target.string() + ": written");
        }
    }
    if (outcome.exit_code == 0) {
        handle_developer_files(module, output_dir, options.check_only, options.verbose, outcome);
    }
    if (drift) {
        outcome.exit_code = 1;
    }
    return outcome;
}

} // namespace

std::vector<DiagnosticRecord> diagnostic_records(const Outcome& outcome) {
    // Every message of the run, in report order, becomes one record. `Message::record` was filled
    // where a real diagnostic was in hand; the rest (setup, usage, drift, developer-file warnings)
    // carry their text and severity only, with no invented span.
    std::vector<DiagnosticRecord> records;
    records.reserve(outcome.messages.size());
    for (const Message& message : outcome.messages) {
        DiagnosticRecord record = message.record;
        if (record.message.empty()) {
            record.message = message.text;
        }
        record.severity = message.is_error ? "error" : "warning";
        records.push_back(std::move(record));
    }
    return records;
}

std::string diagnostics_document(const Options& options, const Outcome& outcome) {
    const std::string primary_file =
        options.inputs.empty() ? std::string() : options.inputs.front();
    return write_diagnostics_json(primary_file, diagnostic_records(outcome));
}

Outcome run(const Options& options) {
    Outcome outcome = run_impl(options);
    if (options.diagnostics_json_path.empty()) {
        return outcome;
    }
    // The document is written even when the run failed, because "why did it fail" is the question
    // the page exists to answer.
    const std::vector<DiagnosticRecord> records = diagnostic_records(outcome);
    const std::string primary_file =
        options.inputs.empty() ? std::string() : options.inputs.front();
    std::string error;
    if (!write_diagnostics_file(options.diagnostics_json_path, primary_file, records, error)) {
        report(outcome, true, "tessera_codegen: " + error);
        outcome.exit_code = 2;
    }
    return outcome;
}

} // namespace tessera::compiler::codegen::cli
