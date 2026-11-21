// SPDX-License-Identifier: MIT

// The front-end stage of the corpus measurement: parse plus the Tier-1 rules. The pipeline stage is
// `measure_pipeline.cpp` and the report text is `report.cpp`, so each unit stays one
// responsibility.

#include "measure.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::corpus {
namespace {

namespace jsx = tessera::compiler::jsx;
namespace rules = tessera::compiler::rules;

void collect(const std::vector<jsx::Diagnostic>& diagnostics, std::vector<std::string>& errors,
             std::vector<std::string>& warnings) {
    for (const jsx::Diagnostic& diagnostic : diagnostics) {
        std::vector<std::string>& target =
            diagnostic.severity == jsx::Severity::Error ? errors : warnings;
        target.emplace_back(diagnostic.code);
    }
}

} // namespace

FileMeasurement measure_source(std::string path, std::string_view text) {
    FileMeasurement measurement;
    measurement.path = std::move(path);

    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    collect(parsed.diagnostics, measurement.errors, measurement.warnings);
    if (parsed.root == nullptr) {
        return measurement;
    }
    for (const jsx::ModuleItem& item : parsed.root->items) {
        if (item.kind == jsx::ItemKind::Component) {
            ++measurement.components;
        }
    }
    // The rules pass needs the map of the same text; it views `text`, which outlives the call.
    const jsx::SourceMap map(text);
    const rules::AnalyzedModule analyzed = rules::analyze_module(*parsed.root, map);
    collect(analyzed.diagnostics, measurement.errors, measurement.warnings);
    measurement.accepted = !parsed.has_errors() && !analyzed.has_errors();
    return measurement;
}

} // namespace tessera::compiler::corpus
