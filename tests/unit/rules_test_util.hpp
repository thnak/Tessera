// SPDX-License-Identifier: MIT

#pragma once

#include "jsx_test_util.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace rules_test {

using namespace jsx_test;
namespace jsx = tessera::compiler::jsx;
namespace rules = tessera::compiler::rules;

// Owns the source, the parse and the analysis. Not movable: the SourceMap points into `source`.
struct Fixture {
    explicit Fixture(std::string text)
        : source(std::move(text)),
          parsed(jsx::parse_module(source)),
          map(source),
          analyzed(rules::analyze_module(*parsed.root, map)) {}
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::string source;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;
};

// Parses and analyses `source`; fails the test when the parser itself reports anything, so a
// fixture cannot pass or fail for a syntax reason.
inline std::unique_ptr<Fixture> analyze(std::string source) {
    auto fixture = std::make_unique<Fixture>(std::move(source));
    check(fixture->parsed.ok(), "fixture parses cleanly: " + codes(fixture->parsed));
    return fixture;
}

// `function A(props) { body return returned; }`.
inline std::string component(std::string_view body, std::string_view returned = "<View/>",
                             std::string_view params = "") {
    return "function A(" + std::string(params) + ") {\n" + std::string(body) + "  return " +
           std::string(returned) + ";\n}\n";
}

// "CODE | line:col | source text of the span | message", the four things a diagnostic test
// asserts. The span text makes the byte range readable; the position checks line and column.
inline std::string describe(const Fixture& fixture, const jsx::Diagnostic& diagnostic) {
    return std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) + ":" +
           std::to_string(diagnostic.span.start.column) + " | " +
           std::string(fixture.map.slice(diagnostic.span.span)) + " | " + diagnostic.message;
}

// Every diagnostic, one per line, in report order.
inline std::string describe_all(const Fixture& fixture) {
    std::string out;
    for (const auto& diagnostic : fixture.analyzed.diagnostics) {
        out += describe(fixture, diagnostic) + "\n";
    }
    return out;
}

inline std::string symbol_text(const rules::ComponentAnalysis& analysis) {
    static const char* const kNames[] = {"Prop", "PropsParam",    "State", "StateSetter",
                                         "Ref",  "AnimatedValue", "Local", "Handler"};
    std::string out;
    for (const auto& symbol : analysis.symbols) {
        out += symbol.name + ":" + kNames[static_cast<int>(symbol.kind)] + " ";
    }
    return out;
}

inline std::string role_text(const rules::ComponentAnalysis& analysis) {
    static const char* const kNames[] = {"NativeHandler", "InlineHandler", "Effect", "Cleanup",
                                         "EventArrow",    "ListCallback",  "Other"};
    std::string out;
    for (const auto& info : analysis.arrows) {
        out += kNames[static_cast<int>(info.role)];
        if (!info.bind_id.empty()) {
            out += "(" + info.bind_id + ")";
        }
        out += " ";
    }
    return out;
}

} // namespace rules_test
