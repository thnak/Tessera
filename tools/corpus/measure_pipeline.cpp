// SPDX-License-Identifier: MIT

// The pipeline stage of the corpus measurement: the whole single-file path the CLI and the preview
// page run (parse, analyse, lower, validate), reporting the first blocking diagnostic.
//
// It calls `collect_source_diagnostics()`, the same entry point the browser's diagnostics half uses
// (tools/codegen/diagnose.cpp), rather than re-running the stages here. That is deliberate: a
// measurement tool that reimplemented the order could disagree with the compiler it measures, and
// the number the ADR records would then be about the tool.

#include "diagnose.hpp"
#include "measure.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::corpus {

void measure_pipeline(FileMeasurement& measurement, std::string_view text) {
    const std::vector<codegen::cli::DiagnosticRecord> records =
        codegen::cli::collect_source_diagnostics(text, measurement.path);
    // The document's convention (docs/specs/compiler-diagnostics-v1.md section 7): a caller that
    // shows one error reports the first. An empty list means the file compiled.
    for (const codegen::cli::DiagnosticRecord& record : records) {
        if (record.severity != "error") {
            continue;
        }
        measurement.pipeline_code = record.code;
        return;
    }
    measurement.pipeline_accepted = true;
}

} // namespace tessera::compiler::corpus
