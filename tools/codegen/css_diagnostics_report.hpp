// SPDX-License-Identifier: MIT

#pragma once

#include "json_text.hpp"
#include "scene_project.hpp"
#include "scene_stylesheet.hpp"

#include <string>
#include <string_view>

// The author stylesheet's diagnostics as one JSON document (issue #390):
//
//   {"cssVersion":2,"styled":true,"truncated":false,
//    "diagnostics":[{"code":"TESSERA_CSS_UNKNOWN_PROPERTY","severity":"warning",
//                    "message":"unsupported property","file":"styles/theme.css",
//                    "line":3,"column":5,"offset":42},...]}
//
// The sheets are the ones the project's entry imports (`ProjectWalk`), so each diagnostic names the
// file it is in and there is no single "the stylesheet" to name at the top. `styled:false` means
// no imported sheet holds CSS, so the page can say the built-in sheet is in force instead of
// showing an empty list that looks like "the sheet is fine".
//
// Bounded at `kMaxReportedCssDiagnostics` records with `truncated` set past it (see
// `scene_stylesheet.hpp`): a stylesheet can have thousands of unknown properties and this document
// is read by a person, not a log sink.
namespace tessera::compiler::codegen::cli {

[[nodiscard]] std::string css_diagnostics_json(const ProjectInput& project);

} // namespace tessera::compiler::codegen::cli
