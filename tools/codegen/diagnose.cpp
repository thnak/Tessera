// SPDX-License-Identifier: MIT

// In-memory diagnostics for the preview page (issue #165, docs/design/compiler-wasm.md section 6).
//
// The stage order here is the CLI's (`run_impl` in regenerate.cpp): parse, analyse, lower,
// validate. It is duplicated on purpose rather than factored into the file-driven driver, because
// the driver's job is a project on disk (tessera.json, import closure, package roots, output files)
// and this one has no filesystem at all. What must not be duplicated is the *diagnostic* path:
// every record below is built by the same `to_record()` and serialized by the same
// `write_diagnostics_json()` the CLI uses, so scripts/ci/wasm_diagnostics_parity.py compares one
// implementation against itself.
//
// Divergences from the CLI are exactly the ones a browser forces, and each is reported the way the
// CLI reports the equivalent situation:
//   - no `tessera.json`, so no salt and no package roots: like an unsalted CLI run, the identity
//     errors `TESSERA_IR_MISSING_ID` for componentId/uiId/automationId are deferred (the CLI
//     filters them too when nothing assigns ids), and a bare package specifier is an ordinary
//     lowering error;
//   - no file reads, so a side-effect `import "./x.css"` has no text and lowering reports
//     `TESSERA_IR_STYLESHEET_MISSING` at the import span, which is what the CLI reports for a
//     stylesheet that does not exist.

#include "diagnose.hpp"
#include "diagnostic_record.hpp"
#include "diagnostics_json.hpp"
#include "project_walk.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::codegen::cli {

namespace {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

// One parsed and analysed in-memory source: the browser's equivalent of the driver's SourceFile.
// The members are ordered so the parse result outlives the SourceMap that views `text`, and the
// analysis that views the parse tree.
struct InMemorySource {
    std::string file;
    std::string text;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;

    InMemorySource(std::string path, std::string source)
        : file(std::move(path)),
          text(std::move(source)),
          parsed(jsx::parse_module(text)),
          map(text),
          analyzed(parsed.root != nullptr ? rules::analyze_module(*parsed.root, map)
                                          : rules::AnalyzedModule{}) {}

    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{file, parsed.root.get(), &analyzed, &map};
    }
};

// The module namespace a single in-memory file implies, matching the CLI's derivation (a lowercased
// identifier-shaped file stem) so the two runs agree on every string that can reach a message.
std::string module_name_for(std::string_view file) {
    const std::size_t slash = file.find_last_of('/');
    std::string stem(file.substr(slash == std::string_view::npos ? 0 : slash + 1));
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        stem.resize(dot);
    }
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

// The CLI's `deferred_identity` rule: with no salt nothing assigns componentId/uiId/automationId,
// so the validator's "missing id" complaints are not diagnostics the user can act on. This is
// normative, not an implementation detail: docs/specs/compiler-diagnostics-v1.md section 7.1 states
// that an unsalted run defers them and that the page runs unsalted, so it matches an unsalted CLI
// run.
//
// The future caller of a salt is P5's share links (a salt in the URL fragment), which must pass it
// to both sides of the parity gate rather than changing what the page reports on its own. Until
// then the parameter is deliberately absent: a salted page would disagree with the CLI for every
// clean input.
bool deferred_identity(const ir::IrDiagnostic& diagnostic) {
    if (diagnostic.code != "TESSERA_IR_MISSING_ID") {
        return false;
    }
    return diagnostic.message.find("componentId") != std::string::npos ||
           diagnostic.message.find("uiId") != std::string::npos ||
           diagnostic.message.find("automationId") != std::string::npos;
}

} // namespace

std::string diagnose_source(std::string_view text, std::string_view file) {
    std::vector<DiagnosticRecord> records;
    const auto report = [&records](const jsx::Diagnostic& diagnostic, std::string_view path) {
        records.push_back(to_record(diagnostic, path));
    };

    InMemorySource source{std::string(file), std::string(text)};

    // Parse, then analyse: the CLI stops at the first file that fails and reports the first
    // diagnostic of that stage, so the page reports the same single error for the same input.
    if (source.parsed.root == nullptr) {
        DiagnosticRecord record;
        record.severity = "error";
        record.message = source.file + ": the file cannot be parsed";
        records.push_back(std::move(record));
        return write_diagnostics_json(file, records);
    }
    if (!source.parsed.diagnostics.empty() && !source.parsed.ok()) {
        report(source.parsed.diagnostics.front(), source.file);
        return write_diagnostics_json(file, records);
    }
    if (source.analyzed.has_errors()) {
        if (!source.analyzed.diagnostics.empty()) {
            report(source.analyzed.diagnostics.front(), source.file);
        } else {
            DiagnosticRecord record;
            record.severity = "error";
            record.message = source.file + ": analysis failed";
            records.push_back(std::move(record));
        }
        return write_diagnostics_json(file, records);
    }

    const lowering::ModuleSource module_source = source.module_source();
    const std::string module_name = module_name_for(file);
    lowering::ModuleLoweringResult lowered = lowering::lower_project(
        std::span<const lowering::ModuleSource>(&module_source, 1), module_name);
    if (!lowered) {
        const lowering::NodeLoweringErrors& errors = lowered.error();
        if (errors.empty()) {
            DiagnosticRecord record;
            record.severity = "error";
            record.message = "lowering failed";
            records.push_back(std::move(record));
        } else {
            report(errors.front(), source.file);
        }
        return write_diagnostics_json(file, records);
    }

    lowering::LoweredModule lowered_module = std::move(*lowered);
    ir::IrModule module;
    module.name = lowered_module.name;
    module.components = std::move(lowered_module.components);
    module.bindings = std::move(lowered_module.bindings);
    module.stylesheets = std::move(lowered_module.stylesheets);

    // No salt in a browser: ids stay unassigned, exactly as in an unsalted CLI run.
    const std::vector<ir::IrDiagnostic> all = ir::validate(module);
    const auto first = std::find_if(all.begin(), all.end(), [](const ir::IrDiagnostic& diagnostic) {
        return !deferred_identity(diagnostic);
    });
    if (first != all.end()) {
        records.push_back(to_record(*first));
    }
    return write_diagnostics_json(file, records);
}

std::string diagnose_project(const ProjectInput& project) {
    const std::string entry = normalize_project_path(project.entry);
    std::vector<DiagnosticRecord> records;
    const auto fail_with = [&](std::string message) {
        DiagnosticRecord record;
        record.severity = "error";
        record.message = std::move(message);
        records.push_back(std::move(record));
        return write_diagnostics_json(entry, records);
    };

    // There is no default entry: a project that names none, or names a file it does not hold, is
    // reported as that, not previewed through some other file.
    if (entry.empty()) {
        return fail_with("the project names no entry file");
    }
    ProjectWalk walk(project);
    if (!walk.has_entry()) {
        return fail_with("the entry `" + entry + "` is not a file of the project");
    }
    if (!walk.run()) {
        const WalkFailure& failure = *walk.failure();
        const WalkUnit* unit = walk.failed_unit();
        if (unit != nullptr && failure.kind == WalkFailure::Kind::Parse &&
            !unit->parsed.diagnostics.empty()) {
            records.push_back(to_record(unit->parsed.diagnostics.front(), failure.file));
            return write_diagnostics_json(entry, records);
        }
        if (unit != nullptr && failure.kind == WalkFailure::Kind::Analyze &&
            !unit->analyzed.diagnostics.empty()) {
            records.push_back(to_record(unit->analyzed.diagnostics.front(), failure.file));
            return write_diagnostics_json(entry, records);
        }
        return fail_with(failure.message);
    }

    std::vector<lowering::ModuleSource> sources;
    sources.reserve(walk.units().size());
    for (const std::unique_ptr<WalkUnit>& unit : walk.units()) {
        sources.push_back(lowering::ModuleSource{unit->path, unit->parsed.root.get(),
                                                 &unit->analyzed, unit->map.get()});
    }
    std::vector<lowering::StylesheetFile> sheets;
    sheets.reserve(walk.sheets().size());
    for (const SheetUse& sheet : walk.sheets()) {
        sheets.push_back(lowering::StylesheetFile{sheet.path, sheet.text});
    }
    lowering::ModuleLoweringResult lowered =
        lowering::lower_project(std::span<const lowering::ModuleSource>(sources),
                                module_name_for(entry), std::span<const lowering::PackageRoot>(),
                                std::span<const lowering::StylesheetFile>(sheets));
    if (!lowered) {
        const lowering::NodeLoweringErrors& errors = lowered.error();
        if (errors.empty()) {
            return fail_with("lowering failed");
        }
        // The lowering says which file its error is in; an error that carries none (a span the
        // type pass could not place) is attributed to the entry, which is the file the page shows.
        const std::string& file = errors.front().file.empty() ? entry : errors.front().file;
        records.push_back(to_record(errors.front(), file));
        return write_diagnostics_json(entry, records);
    }

    lowering::LoweredModule lowered_module = std::move(*lowered);
    ir::IrModule module;
    module.name = lowered_module.name;
    module.components = std::move(lowered_module.components);
    module.bindings = std::move(lowered_module.bindings);
    module.stylesheets = std::move(lowered_module.stylesheets);

    const std::vector<ir::IrDiagnostic> all = ir::validate(module);
    const auto first = std::find_if(all.begin(), all.end(), [](const ir::IrDiagnostic& diagnostic) {
        return !deferred_identity(diagnostic);
    });
    if (first != all.end()) {
        records.push_back(to_record(*first));
    }
    return write_diagnostics_json(entry, records);
}

} // namespace tessera::compiler::codegen::cli
