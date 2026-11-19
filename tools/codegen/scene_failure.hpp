// SPDX-License-Identifier: MIT

#pragma once

#include "diagnostics_json.hpp"

#include <string>
#include <string_view>

// What the native preview CLIs print when a source produced no scene. The document contract keeps
// failure *in* the document (an empty `nodes` array, scene_preview.hpp), which is right for a
// machine and silent for a person: a source using `useState<number>(0)` exited 1 with nothing on
// either stream. This is the loud half. It reuses `collect_source_diagnostics()` (the records the
// diagnostics document is made of), so the printed code, span and hint are the ones the compiler
// CLI and the Studio's diagnostics panel report, never a second derivation.
namespace tessera::compiler::codegen::cli {

// One record as a single stderr line:
//   FILE:LINE:COL: error TESSERA_CODE: message (hint: suggestion)
// A record with no location prints `FILE: error ...`; an empty suggestion drops the hint clause.
[[nodiscard]] std::string format_record_line(const DiagnosticRecord& record,
                                             std::string_view fallback_file);

// The newline-terminated lines for `source` (as the file `file`): one per diagnostic. When the
// compiler front end has nothing to say (the failure is later, in layout or scene building) the one
// line is `FILE: error: <scene_error>`, so a failed run never prints nothing.
[[nodiscard]] std::string scene_failure_text(std::string_view source, std::string_view file,
                                             std::string_view scene_error);

} // namespace tessera::compiler::codegen::cli
