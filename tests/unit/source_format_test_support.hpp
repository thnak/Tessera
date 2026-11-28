// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/format/format.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace source_format_test {

inline int& failures() {
    static int count = 0;
    return count;
}

inline void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures();
        std::cerr << "FAIL: " << what << "\n";
    }
}

inline tessera::compiler::format::FormatResult format_tsx(std::string_view text) {
    return tessera::compiler::format::format_source(tessera::compiler::format::FileKind::Tsx, text);
}

// The three properties of ADR-061 section 2 for one input that must be formatted: idempotent,
// meaning-preserving (same AST ignoring trivia) and no comment lost. Records a failure naming
// `what` and returns the formatted text, or an empty string when the input was refused.
std::string check_properties(std::string_view text, const std::string& what);

// Defined in source_format_corpus_tests.cpp.
void run_corpus_tests();
// Defined in source_format_edits_tests.cpp.
void run_edits_tests();
// Defined in source_format_css_tests.cpp.
void run_css_json_tests();
// Defined in source_format_recover_tests.cpp.
void run_recover_tests();
// Defined in source_format_cli_tests.cpp.
void run_cli_tests();

} // namespace source_format_test
