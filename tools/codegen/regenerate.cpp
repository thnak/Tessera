// SPDX-License-Identifier: MIT

// Codegen CLI driver: read .tsx -> parse/analyse -> lower -> validate IR -> generate C++ -> merge
// with the existing files and rewrite only what changed (docs/guides/developer-workflow.md).

#include "emit_library_components.hpp"
#include "project_identity.hpp"
#include "regenerate.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/merger.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/tokenizer.hpp"

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
#include <vector>

namespace tessera::compiler::codegen::cli {

using internal::import_target;
using internal::PackageRootSpec;
using internal::ProjectIdentity;
using internal::read_file;
using internal::report;
using internal::resolve_identity;

namespace {

namespace fs = std::filesystem;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace style = tessera::ui::style;

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

std::string ir_diagnostic_text(const ir::IrDiagnostic& diagnostic) {
    const ir::SourceSpan& span = diagnostic.span;
    std::string text;
    if (!span.file.empty()) {
        text +=
            span.file + ":" + std::to_string(span.line) + ":" + std::to_string(span.column) + ": ";
    }
    return text + diagnostic.code + ": " + diagnostic.message;
}

// `path:line:column` for a byte offset into a stylesheet: the engine reports offsets, the CLI's
// contract with the user is file:line:column (docs/guides/developer-workflow.md).
std::string css_diagnostic_text(std::string_view path, std::string_view text, std::size_t offset,
                                std::string_view message) {
    std::size_t line = 1;
    std::size_t column = 1;
    const std::size_t end = std::min(offset, text.size());
    for (std::size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return std::string(path) + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " +
           std::string(message);
}

// The stylesheet parser is tolerant by design (stylesheet-v1.md section 10), so a build-time
// failure can only come from the lexical error it proves: an unterminated string. Everything else
// prepares, and the prepared sheet carries its own diagnostics for the run time.
std::optional<std::string> stylesheet_syntax_error(std::string_view path, std::string_view text) {
    style::Tokenizer tokenizer(text);
    while (true) {
        const style::Token token = tokenizer.next();
        if (token.type == style::TokenType::Eof) {
            return std::nullopt;
        }
        if (token.type == style::TokenType::BadString) {
            return css_diagnostic_text(path, text, token.offset,
                                       "TESSERA_CSS_SYNTAX: unterminated string in the stylesheet");
        }
    }
}

// Compiled stylesheet blobs of the module (issue #81, ADR-048): the CLI compiles the CSS it read,
// so `StylesheetEmbed::bytes` is exactly `serialize_stylesheet()` output and the codegen library
// never sees a stylesheet engine header.
std::optional<std::vector<StylesheetEmbed>>
build_stylesheet_embeds(std::span<const std::string> paths, std::span<const std::string> texts,
                        Outcome& outcome) {
    std::vector<StylesheetEmbed> embeds;
    embeds.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (const std::optional<std::string> error = stylesheet_syntax_error(paths[i], texts[i])) {
            report(outcome, true, *error);
            outcome.exit_code = 1;
            return std::nullopt;
        }
        const style::PreparedStyleSheet prepared = style::prepare_stylesheet(texts[i]);
        embeds.push_back(StylesheetEmbed{paths[i], style::serialize_stylesheet(prepared)});
    }
    return embeds;
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
                    std::vector<GeneratedFile>& files, Outcome& outcome, std::string_view input) {
    const auto generated = generate(module, embeds);
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

} // namespace

Outcome run(const Options& options) {
    Outcome outcome;
    if (options.inputs.empty()) {
        report(outcome, true, "tessera_codegen: no input .tsx file");
        outcome.exit_code = 2;
        return outcome;
    }
    if (!options.emit_ir_path.empty() && !options.emit_library_cpp_path.empty()) {
        report(outcome, true, "tessera_codegen: choose one of --emit-ir and --emit-library-cpp");
        outcome.exit_code = 2;
        return outcome;
    }
    const bool emits = !options.emit_ir_path.empty() || !options.emit_library_cpp_path.empty();
    if (!emits && options.output_dir.empty()) {
        report(outcome, true, "tessera_codegen: --out is required");
        outcome.exit_code = 2;
        return outcome;
    }

    const std::optional<ProjectIdentity> identity = resolve_identity(options, outcome);
    if (!identity) {
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
                fs::relative(fs::absolute(fs::path(input)), root, relative_error);
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
            pending.emplace_back(std::move(path), fs::path(input));
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
            if (source->parsed.root == nullptr) {
                report(outcome, true, source->path + ": the file cannot be parsed");
            } else if (!source->parsed.diagnostics.empty()) {
                report(outcome, true,
                       diagnostic_text(source->path, source->parsed.diagnostics.front()));
            } else {
                report(outcome, true, source->path + ": parse failed");
            }
            outcome.exit_code = 1;
            return outcome;
        }
        if (source->analyzed.has_errors()) {
            if (!source->analyzed.diagnostics.empty()) {
                report(outcome, true,
                       diagnostic_text(source->path, source->analyzed.diagnostics.front()));
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
                project_paths ? identity->root / fs::path(path)
                              : (fs::path(source->path).parent_path() / fs::path(item.source))
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
        report(outcome, true,
               errors.empty() ? "lowering failed"
                              : diagnostic_text(sources.front()->path, errors.front()));
        outcome.exit_code = 1;
        return outcome;
    }

    lowering::LoweredModule lowered_module = std::move(*lowered);
    ir::IrModule module;
    module.name = lowered_module.name;
    module.components = std::move(lowered_module.components);
    module.bindings = std::move(lowered_module.bindings);
    module.stylesheets = std::move(lowered_module.stylesheets);

    if (!identity->salt.empty()) {
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
    const bool identity_assigned = !identity->salt.empty();
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
        report(outcome, true, ir_diagnostic_text(*first_ir));
        outcome.exit_code = 1;
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
    if (!generate_files(module, *embeds, files, outcome, options.inputs.front())) {
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
            tessera::compiler::MergeResult merged = tessera::compiler::merge(file.text, *existing);
            for (const std::string& warning : merged.warnings) {
                report(outcome, false, target.string() + ": warning: " + warning);
            }
            content = std::move(merged.text);
            if (content == *existing) {
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
    if (drift) {
        outcome.exit_code = 1;
    }
    return outcome;
}

} // namespace tessera::compiler::codegen::cli
