// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

// One JSON string literal, escaped as the format requires (issue #433 split this out of
// `css_diagnostics_report.hpp`).
//
// **Why it is its own header.** The escape table is needed by every document this project's modules
// write, and it has no dependency beyond `<string>`: a serializer that needs nothing but `quoted()`
// used to pull in `css_diagnostics_report.hpp`, which includes the stylesheet and project headers
// and therefore the whole lowering stack. That made a pure `WindowMetrics` serializer untestable
// without linking the compiler. One rule, one function, in a file that costs nothing to include.
//
// The only text that reaches a report is this project's own ASCII diagnostics and vocabulary words,
// so escaping quotes, backslashes and control characters is the whole job; a general encoder would
// be a second implementation of a solved problem.
namespace tessera::compiler::codegen::cli {

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

} // namespace tessera::compiler::codegen::cli
