// SPDX-License-Identifier: MIT

// Shared helpers for the lowering_* tests: parse an expression, lower it, and inspect the result.

#pragma once

#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/expression_lowering.hpp"
#include "tessera/compiler/lowering/name_resolver.hpp"

#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lowering_test {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;

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

// A name resolver that stands in for the analysis pass of slice 3 and the type table of slice 4.
class MapNames final : public lowering::NameResolver {
public:
    MapNames(std::initializer_list<std::pair<std::string, std::string>> entries)
        : entries_(entries) {}

    [[nodiscard]] std::optional<std::string> resolve(std::string_view name) const override {
        for (const auto& entry : entries_) {
            if (entry.first == name) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

struct Lowered {
    jsx::ExpressionParseResult parsed;
    lowering::ExpressionLoweringResult result;
};

// Parses and lowers one expression. `source` only has to outlive the call: parser spans are
// resolved into the diagnostics, and the IR owns its strings.
inline Lowered lower(std::string_view source, const lowering::NameResolver* names = nullptr,
                     std::string_view file = "ui/Counter.tsx") {
    Lowered lowered;
    lowered.parsed = jsx::parse_expression(source);
    if (!lowered.parsed.root) {
        lowered.result = std::unexpected(lowering::ExpressionLoweringErrors{});
        return lowered;
    }
    const jsx::SourceMap map(source);
    const lowering::ExpressionLoweringContext context{map, file, names};
    lowered.result = lowering::lower_expression(*lowered.parsed.root, context);
    return lowered;
}

// Null when lowering failed.
inline const ir::Expr* expr_of(const Lowered& lowered) {
    return lowered.result ? lowered.result->get() : nullptr;
}

inline std::string ir_text(const ir::Expr& expr) {
    return ir::to_text(ir::expr_to_json(&expr));
}

inline std::string first_code(const lowering::ExpressionLoweringResult& result) {
    if (result || result.error().empty()) {
        return {};
    }
    return std::string(result.error().front().code);
}

inline jsx::Diagnostic first_diagnostic(const lowering::ExpressionLoweringResult& result) {
    if (result || result.error().empty()) {
        return jsx::Diagnostic{"<none>", jsx::Severity::Error, {}, {}, {}, std::nullopt};
    }
    return result.error().front();
}

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

inline int finish(const char* name) {
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << name << " passed\n";
    return 0;
}

} // namespace lowering_test
