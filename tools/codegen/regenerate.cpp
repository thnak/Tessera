// SPDX-License-Identifier: MIT

// Codegen CLI driver: read .tsx -> parse/analyse -> lower -> validate IR -> generate C++ -> merge
// with the existing files and rewrite only what changed (docs/guides/developer-workflow.md).

#include "emit_library_components.hpp"
#include "package_manifest.hpp"
#include "project_identity.hpp"
#include "regenerate.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/library_components.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/merger.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/style/component_parts.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/theme_asset.hpp"
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
#include <variant>
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

// The parts a component source declares: every literal `part` attribute of its static node tree
// (stylesheet-v1 section 6.3). A pre-order walk, so the collected set is deterministic.
void collect_declared_parts(const ir::Node& node, std::set<std::string>& out) {
    const auto found = node.props.find("part");
    if (found != node.props.end() && found->second) {
        const ir::Expr& value = *found->second;
        if (value.kind == ir::ExprKind::Literal &&
            std::holds_alternative<std::string>(value.literal)) {
            const std::string& text = std::get<std::string>(value.literal);
            for (std::size_t index = 0;; ++index) {
                const std::string_view name = style::part_name_at(text, index);
                if (name.empty()) {
                    break;
                }
                out.emplace(name);
            }
        }
    }
    for (const ir::Node& child : node.children) {
        collect_declared_parts(child, out);
    }
    if (node.fallback) {
        collect_declared_parts(*node.fallback, out);
    }
    if (node.item_template) {
        collect_declared_parts(*node.item_template, out);
    }
}

// The part declarations of the library components: their part names are the only published styling
// contract, so they are the only ones `::part()` selectors are checked against (ADR-047 item 3).
// App components are absent by construction, which is why they are never diagnosed.
style::ComponentPartRegistry library_part_registry() {
    style::ComponentPartRegistry registry;
    for (const lowering::LibraryComponent& component : lowering::library_components()) {
        std::set<std::string> names;
        collect_declared_parts(lowering::make_library_component(component).root, names);
        std::vector<std::string_view> parts;
        parts.reserve(names.size());
        for (const std::string& name : names) {
            parts.push_back(name);
        }
        registry.declare(std::string(component.name), parts);
    }
    return registry;
}

// Compiled stylesheet blobs of the module (issue #81, ADR-048): the CLI compiles the CSS it read,
// so `StylesheetEmbed::bytes` is exactly `serialize_stylesheet()` output and the codegen library
// never sees a stylesheet engine header.
std::optional<std::vector<StylesheetEmbed>>
build_stylesheet_embeds(std::span<const std::string> paths, std::span<const std::string> texts,
                        Outcome& outcome) {
    std::vector<StylesheetEmbed> embeds;
    embeds.reserve(paths.size());
    const style::ComponentPartRegistry parts = library_part_registry();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (const std::optional<std::string> error = stylesheet_syntax_error(paths[i], texts[i])) {
            report(outcome, true, *error);
            outcome.exit_code = 1;
            return std::nullopt;
        }
        const style::PreparedStyleSheet prepared = style::prepare_stylesheet(texts[i]);
        // `::part(name)` on a library component that does not declare `name` is a warning, not an
        // error: the rule simply never matches (stylesheet-v1 section 6.3).
        for (const style::StyleDiagnostic& diagnostic :
             style::check_part_selectors(prepared, parts)) {
            report(outcome, false,
                   css_diagnostic_text(paths[i], texts[i], diagnostic.offset,
                                       "TESSERA_CSS_UNDECLARED_PART: " + diagnostic.message));
        }
        embeds.push_back(StylesheetEmbed{paths[i], style::serialize_stylesheet(prepared)});
    }
    return embeds;
}

bool safe_file_name(std::string_view name) {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find("..") == std::string_view::npos;
}

// The embedded theme asset (issue #81, ADR-048): a project without `assets.theme` has no asset and
// therefore no generated theme file. A configured asset is parsed and serialized once here, so the
// codegen library never sees the JSON or the style engine (docs/specs/stylesheet-v1.md 9.1).
bool build_theme_embed(const ProjectIdentity& identity, Outcome& outcome,
                       std::optional<EmbeddedTheme>& theme) {
    if (identity.theme.empty()) {
        return true;
    }
    const std::optional<std::string> text = read_file(identity.root / fs::path(identity.theme));
    if (!text) {
        report(outcome, true, identity.theme + ": TESSERA_THEME_READ: cannot read the theme asset");
        outcome.exit_code = 1;
        return false;
    }
    const auto parsed = style::parse_theme_asset(*text);
    if (!parsed) {
        report(outcome, true,
               identity.theme + ": " + style::theme_error_code_name(parsed.error().code) + ": " +
                   parsed.error().message);
        outcome.exit_code = 1;
        return false;
    }
    for (const std::string& warning : parsed->warnings) {
        report(outcome, false, identity.theme + ": warning: " + warning);
    }
    theme = EmbeddedTheme{style::serialize_theme_asset(*parsed)};
    return true;
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
                    const std::optional<EmbeddedTheme>& theme, std::vector<GeneratedFile>& files,
                    Outcome& outcome, std::string_view input, bool generated_base) {
    const auto generated = generate(module, embeds, theme,
                                    generated_base ? EmitMode::GeneratedBase : EmitMode::Legacy);
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
    if (!generate_files(module, *embeds, theme, files, outcome, options.inputs.front(),
                        options.generated_base)) {
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
        if (existing && options.generated_base) {
            // The generated base is regenerated wholesale: no regions, nothing to merge.
            if (*existing == content) {
                if (options.verbose) {
                    report(outcome, false, target.string() + ": unchanged");
                }
                continue;
            }
        } else if (existing) {
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
    if (options.generated_base && outcome.exit_code == 0) {
        handle_developer_files(module, output_dir, options.check_only, options.verbose, outcome);
    }
    if (drift) {
        outcome.exit_code = 1;
    }
    return outcome;
}

} // namespace tessera::compiler::codegen::cli
