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

    // The same options the CLI and the preview use, so the measurement answers "does the port
    // compile" for the pipeline a user actually runs. With `custom_hooks` on (the default since
    // #645) a project-declared `use...` function is expanded before the rules see it.
    const jsx::FrontEndOptions options;
    const jsx::ModuleParseResult parsed = jsx::parse_module(text, jsx::Limits{}, options);
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
    // The expansion runs before the rules (docs/specs/custom-hooks-v1.md section 4), so the
    // measurement must run it too: without it a custom hook is measured as an unknown hook, which
    // is the pipeline the front end no longer has.
    jsx::DiagnosticSink expansion_sink(map, jsx::Limits{});
    const jsx::ExpansionMetrics expansion =
        jsx::expand_custom_hooks(*parsed.root, map, options, expansion_sink);
    collect(expansion_sink.diagnostics(), measurement.errors, measurement.warnings);
    const rules::AnalyzedModule analyzed =
        rules::analyze_module(*parsed.root, map, jsx::Limits{}, nullptr, &expansion.call_sites);
    collect(analyzed.diagnostics, measurement.errors, measurement.warnings);
    measurement.accepted =
        !parsed.has_errors() && !analyzed.has_errors() && expansion_sink.diagnostics().empty();
    return measurement;
}

} // namespace tessera::compiler::corpus
