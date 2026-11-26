// SPDX-License-Identifier: MIT

// The IR half of the compile-only surface. It mirrors the stage order of
// `tools/codegen/diagnose.cpp` (parse -> analyse -> lower -> validate) because it must reach the
// same module the CLI's `--emit-ir` writes for the same single file; the difference is only what it
// returns. Duplicating four calls is the price of not changing the page's diagnostics driver, and
// the test drives both over one corpus so the two cannot drift silently.

#include "in_memory_ir.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/compiler/services/manifest.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::mcp {
namespace {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

// The same derivation as the diagnostics driver: a lowercased identifier-shaped stem. Both sides
// must agree because the module name reaches the IR (and therefore `module.name` on the wire).
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
        const bool identifier = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') || c == '_';
        name += identifier ? static_cast<char>(ch >= 'A' && ch <= 'Z' ? c - 'A' + 'a' : c) : '_';
    }
    if (name.empty()) {
        name = "module";
    }
    if (name.front() >= '0' && name.front() <= '9') {
        name.insert(name.begin(), '_');
    }
    return name;
}

// The unsalted-run rule of docs/specs/compiler-diagnostics-v1.md section 7.1: with no id salt the
// validator's "missing id" complaints are not actionable, so the page (and this adapter, which has
// no tessera.json) defers them. Keep this identical to `tools/codegen/diagnose.cpp`.
bool deferred_identity(const ir::IrDiagnostic& diagnostic) {
    if (diagnostic.code != "TESSERA_IR_MISSING_ID") {
        return false;
    }
    return diagnostic.message.find("componentId") != std::string::npos ||
           diagnostic.message.find("uiId") != std::string::npos ||
           diagnostic.message.find("automationId") != std::string::npos;
}

} // namespace

std::optional<std::string> lower_source_to_ir_json(std::string_view text, std::string_view file,
                                                   std::string_view services_manifest) {
    // Parsed first: the analysis points into it, so it must outlive the analysis.
    std::optional<tessera::compiler::services::ServicesManifest> manifest;
    if (!services_manifest.empty()) {
        auto parsed_manifest = tessera::compiler::services::parse_services_manifest(services_manifest);
        if (!parsed_manifest) {
            return std::nullopt;
        }
        manifest = std::move(*parsed_manifest);
    }
    // The parse result views `text`, and the analysis views the parse tree, so the locals are
    // ordered source -> parse -> map -> analysis exactly as the diagnostics driver orders them.
    const std::string source(text);
    jsx::ModuleParseResult parsed = jsx::parse_module(source);
    if (parsed.root == nullptr || !parsed.diagnostics.empty()) {
        return std::nullopt;
    }
    const jsx::SourceMap map(source);
    const rules::AnalyzedModule analyzed = rules::analyze_module(
        *parsed.root, map, jsx::Limits{}, nullptr, nullptr, manifest ? &*manifest : nullptr);
    if (analyzed.has_errors()) {
        return std::nullopt;
    }

    const lowering::ModuleSource module_source{file, parsed.root.get(), &analyzed, &map};
    lowering::ModuleLoweringResult lowered = lowering::lower_project(
        std::span<const lowering::ModuleSource>(&module_source, 1), module_name_for(file));
    if (!lowered) {
        return std::nullopt;
    }

    lowering::LoweredModule lowered_module = std::move(*lowered);
    ir::IrModule module;
    module.name = lowered_module.name;
    module.components = std::move(lowered_module.components);
    module.bindings = std::move(lowered_module.bindings);
    module.stylesheets = std::move(lowered_module.stylesheets);
    module.types = std::move(lowered_module.types);

    // Unassigned ids are recorded through the same test the errors are, so asking for IR never
    // circumnavigates a validation error the diagnostics tool would report.
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        if (!deferred_identity(diagnostic)) {
            return std::nullopt;
        }
    }
    return ir::write_ir_json(module);
}

} // namespace tessera::mcp
