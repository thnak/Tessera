// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/parser.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace jsx_test {

inline int failures = 0;

inline void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

inline void check_eq(const std::string& actual, const std::string& expected,
                     std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

// "CODE1;CODE2;" in report order.
template<typename Result> std::string codes(const Result& result) {
    std::string out;
    for (const auto& diagnostic : result.diagnostics) {
        out += std::string(diagnostic.code) + ";";
    }
    return out;
}

// The first diagnostic, or an empty one, so a broken parser fails a check instead of indexing an
// empty vector.
template<typename Result> tessera::compiler::jsx::Diagnostic first_diag(const Result& result) {
    if (result.diagnostics.empty()) {
        return tessera::compiler::jsx::Diagnostic{
            "<none>", tessera::compiler::jsx::Severity::Error, {}, {}, {}, std::nullopt};
    }
    return result.diagnostics.front();
}

inline std::string dump_expr(std::string_view source) {
    const auto result = tessera::compiler::jsx::parse_expression(source);
    return result.root ? tessera::compiler::jsx::dump(*result.root) : "<null>";
}

inline std::string dump_jsx(std::string_view source) {
    const auto result = tessera::compiler::jsx::parse_jsx_element(source);
    return result.root ? tessera::compiler::jsx::dump(*result.root) : "<null>";
}

inline int finish(const char* name) {
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << name << " passed\n";
    return 0;
}

} // namespace jsx_test
