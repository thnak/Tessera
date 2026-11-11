// SPDX-License-Identifier: MIT

// Codegen CLI driver: read .tsx -> parse/analyse -> lower -> validate IR -> generate C++ -> merge
// with the existing files and rewrite only what changed (docs/guides/developer-workflow.md).

#include "regenerate.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/merger.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/harness/json_parse.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
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

void report(Outcome& outcome, bool is_error, std::string text) {
    outcome.messages.push_back(Message{is_error, std::move(text)});
    if (is_error) {
        outcome.exit_code = outcome.exit_code == 0 ? 1 : outcome.exit_code;
    }
}

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

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
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

// The project identity of this run: the id salt (empty: ids stay unassigned) and the root that
// source paths are relative to (docs/specs/automation-ids-v1.md section 2, project-config-v1.md
// `app.namespace`). The salt is `--salt`, else `app.namespace` of the `tessera.json` at `--root`
// or found by walking up from the first input; a `tessera.json` that is found but has no usable
// namespace is an error, not a silent skip.
// One package root of the project config (docs/specs/project-config-v1.md `packages`, issue #70): a
// bare specifier resolves inside it, so a TSX component library is compiled by the same pipeline.
struct PackageRootSpec {
    std::string name;
    std::string root; // project-relative POSIX directory
};

struct ProjectIdentity {
    std::string salt;
    fs::path root;
    std::vector<PackageRootSpec> packages;
};

// Reads the optional `packages` array: every entry is `{ "name": string, "root": string }` with a
// unique name and a project-relative directory (no absolute path, drive letter or `..` escape).
// Returns false and sets `why` on a malformed entry; an absent/null `packages` is valid.
bool read_packages(const tessera::harness::JsonValue& config, ProjectIdentity& identity,
                   std::string& why) {
    const tessera::harness::JsonValue* packages = config.find("packages");
    if (packages == nullptr || packages->is_null()) {
        return true;
    }
    const tessera::harness::JsonValue::Array* entries = packages->as_array();
    if (entries == nullptr) {
        why = "`packages` must be an array of { \"name\", \"root\" } objects";
        return false;
    }
    for (const tessera::harness::JsonValue& entry : *entries) {
        const tessera::harness::JsonValue* name_node = entry.find("name");
        const tessera::harness::JsonValue* root_node = entry.find("root");
        const std::optional<std::string_view> name =
            name_node != nullptr ? name_node->as_string() : std::nullopt;
        const std::optional<std::string_view> root_value =
            root_node != nullptr ? root_node->as_string() : std::nullopt;
        if (!name || name->empty() || !root_value || root_value->empty()) {
            why = "every `packages` entry needs a non-empty `name` and `root`";
            return false;
        }
        if (name->find('/') != std::string_view::npos ||
            name->find('\\') != std::string_view::npos) {
            why = "package name `" + std::string(*name) + "` must not contain a path separator";
            return false;
        }
        std::string root(*root_value);
        std::replace(root.begin(), root.end(), '\\', '/');
        while (root.size() > 1 && root.back() == '/') {
            root.pop_back();
        }
        const bool absolute = root.front() == '/' || (root.size() > 1 && root[1] == ':');
        const bool escapes = root == ".." || root == "." || root.starts_with("../") ||
                             root.find("/../") != std::string::npos || root.ends_with("/..");
        if (absolute || escapes) {
            why = "package root `" + root + "` must be a project-relative directory";
            return false;
        }
        for (const PackageRootSpec& existing : identity.packages) {
            if (existing.name == *name) {
                why = "duplicate package name `" + std::string(*name) + "`";
                return false;
            }
        }
        identity.packages.push_back(PackageRootSpec{std::string(*name), std::move(root)});
    }
    return true;
}

std::optional<fs::path> find_config(const fs::path& start) {
    for (fs::path dir = start;; dir = dir.parent_path()) {
        std::error_code error;
        if (fs::is_regular_file(dir / "tessera.json", error)) {
            return dir / "tessera.json";
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return std::nullopt;
        }
    }
}

std::optional<ProjectIdentity> resolve_identity(const Options& options, Outcome& outcome) {
    const fs::path first = fs::absolute(fs::path(options.inputs.front())).parent_path();
    const std::optional<fs::path> config =
        find_config(options.root.empty() ? first : fs::absolute(fs::path(options.root)));
    ProjectIdentity identity;
    identity.salt = options.salt;
    if (!options.root.empty()) {
        identity.root = fs::absolute(fs::path(options.root));
    } else {
        identity.root = config ? config->parent_path() : first;
    }
    if (!identity.salt.empty() || !config) {
        // A config can still declare package roots without providing the salt, so the file is read
        // below whenever it exists.
        if (!config) {
            return identity;
        }
    }
    const auto fail = [&](const std::string& why) {
        report(outcome, true, "tessera_codegen: " + config->generic_string() + ": " + why);
        outcome.exit_code = 1;
        return std::optional<ProjectIdentity>();
    };
    const std::optional<std::string> text = read_file(*config);
    if (!text) {
        return fail("cannot read the file");
    }
    const auto parsed = tessera::harness::parse_json(*text);
    if (!parsed) {
        return fail("not valid JSON (offset " + std::to_string(parsed.error().offset) + ")");
    }
    std::string package_error;
    if (!read_packages(*parsed, identity, package_error)) {
        return fail(package_error);
    }
    if (!identity.salt.empty()) {
        return identity;
    }
    const tessera::harness::JsonValue* app = parsed->find("app");
    const tessera::harness::JsonValue* name = app != nullptr ? app->find("namespace") : nullptr;
    const std::optional<std::string_view> value =
        name != nullptr ? name->as_string() : std::nullopt;
    if (!value || value->empty()) {
        return fail(
            "`app.namespace` is required and must be a non-empty string: it is the id salt");
    }
    identity.salt = std::string(*value);
    return identity;
}

bool generate_files(const ir::IrModule& module, std::vector<GeneratedFile>& files, Outcome& outcome,
                    std::string_view input) {
    const auto generated = generate(module);
    if (!generated) {
        const CodegenErrors& errors = generated.error();
        report(outcome, true,
               std::string(input) + ": " +
                   (errors.empty() ? std::string("codegen failed") : errors.front()));
        outcome.exit_code = 1;
        return false;
    }
    files = *generated;
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
    if (options.output_dir.empty()) {
        report(outcome, true, "tessera_codegen: --out is required");
        outcome.exit_code = 2;
        return outcome;
    }

    const std::optional<ProjectIdentity> identity = resolve_identity(options, outcome);
    if (!identity) {
        return outcome;
    }

    std::vector<std::unique_ptr<SourceFile>> sources;
    for (const std::string& input : options.inputs) {
        std::optional<std::string> text = read_file(fs::path(input));
        if (!text) {
            report(outcome, true, "tessera_codegen: cannot read `" + input + "`");
            outcome.exit_code = 2;
            return outcome;
        }
        // With a salt the path feeds the ids, and with package roots it must be project-relative so
        // `pkg/Sub` matches `<root>/Sub`; either way the form is POSIX and checkout-independent.
        std::string path = input;
        if (!identity->salt.empty() || !identity->packages.empty()) {
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
        sources.push_back(std::make_unique<SourceFile>(path, std::move(*text)));
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
    lowering::ModuleLoweringResult lowered = lowering::lower_project(
        std::span<const lowering::ModuleSource>(module_sources), module_name, packages);
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

    std::vector<GeneratedFile> files;
    if (!generate_files(module, files, outcome, options.inputs.front())) {
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
