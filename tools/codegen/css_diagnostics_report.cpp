// SPDX-License-Identifier: MIT
//
// The author stylesheet's diagnostics document (issue #390).
//
// **Why this is its own export rather than a field of the snapshot.** The snapshot document is the
// P2 parity artifact and `scripts/ci/scene_snapshot_parity.py` pins its bytes; a stylesheet problem
// is not part of the tree, so adding a member there would make every corpus case differ for a
// reason unrelated to what the gate compares. The diagnostics module cannot carry these either: it
// links no stylesheet engine by design (it exists to stay small and engine-free), so the module
// that already compiled the sheet is the one that reports it.
//
// **Why it is its own translation unit rather than another function in `wasm_scene_entry.cpp`.**
// Two reasons, both about the file being a file: that entry is at the 500-line hard limit, and the
// document writer is `canvas_report.cpp`'s job already (`canvas_report_json` writes the same
// records into a frame report). One serializer for one shape of record.
//
// Nothing here reads the filesystem, allocates a device or touches a clock: it is a pure function
// of its two arguments, which is what makes it usable from the browser module and testable
// natively.

#include "css_diagnostics_report.hpp"
#include "project_walk.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::codegen::cli {

std::string css_diagnostics_json(const ProjectInput& project) {
    // The sheets the entry reaches, even when the walk stopped partway on a broken file: the sheets
    // collected before the stop are still the author's, and their problems are worth reporting.
    ProjectWalk walk(project);
    if (walk.has_entry()) {
        (void)walk.run();
    }
    const PreviewSheets prepared =
        prepare_preview_sheets(walk.sheets(), kMaxReportedCssDiagnostics);
    const std::vector<PreviewCssDiagnostic>& problems = prepared.diagnostics;

    std::string out = "{\"cssVersion\":2,\"styled\":";
    out += prepared.prepared.author() ? "true" : "false";
    // `truncated` rather than a count: a page that got `kMaxReportedCssDiagnostics` records cannot
    // tell "exactly the cap" from "more than the cap", and the difference changes what it should
    // say.
    out += ",\"truncated\":";
    out += problems.size() == kMaxReportedCssDiagnostics ? "true" : "false";
    out += ",\"diagnostics\":[";
    for (std::size_t index = 0; index < problems.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        const PreviewCssDiagnostic& problem = problems[index];
        out += "{\"code\":";
        out += quoted(problem.code);
        out += ",\"severity\":";
        out += quoted(problem.warning ? "warning" : "error");
        out += ",\"message\":";
        out += quoted(problem.message);
        out += ",\"file\":";
        out += quoted(problem.file);
        out += ",\"line\":";
        out += std::to_string(problem.line);
        out += ",\"column\":";
        out += std::to_string(problem.column);
        out += ",\"offset\":";
        out += std::to_string(problem.offset);
        out.push_back('}');
    }
    out += "]}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
