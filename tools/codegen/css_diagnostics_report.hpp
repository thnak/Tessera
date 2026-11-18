// SPDX-License-Identifier: MIT

#pragma once

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

// One JSON string literal, escaped as the format requires. The only text that reaches either report
// is this project's own ASCII diagnostics, so escaping quotes, backslashes and control characters
// is the whole job; a general encoder would be a second implementation of a solved problem.
[[nodiscard]] inline std::string quoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char character : text) {
        const auto code = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (code < 0x20U) {
                // U+0000..U+001F must not appear raw in a JSON string.
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[code >> 4U]);
                out.push_back(kHex[code & 0x0FU]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string css_diagnostics_json(const ProjectInput& project);

} // namespace tessera::compiler::codegen::cli
