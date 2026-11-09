// SPDX-License-Identifier: MIT

// Shared helpers for the lowering_nodes_* tests: parse a component, run the analysis pass, lower
// hooks and nodes (slice 7), and describe the resulting IR tree.

#pragma once

#include "jsx_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lowering_nodes_test {

using namespace jsx_test;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace rules = tessera::compiler::rules;
namespace lowering = tessera::compiler::lowering;

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

inline std::unique_ptr<Fixture> parse(std::string text) {
    auto fixture = std::make_unique<Fixture>(std::move(text));
    check(fixture->parsed.ok(), "fixture parses cleanly: " + codes(fixture->parsed));
    check(fixture->analyzed.diagnostics.empty(),
          "fixture analyses cleanly: " +
              (fixture->analyzed.diagnostics.empty()
                   ? std::string()
                   : std::string(fixture->analyzed.diagnostics[0].message)));
    return fixture;
}

// Parses only; for fixtures the analysis pass rejects.
inline std::unique_ptr<Fixture> parse_raw(std::string text) {
    auto fixture = std::make_unique<Fixture>(std::move(text));
    check(fixture->parsed.root != nullptr, "the raw fixture parses");
    return fixture;
}

// `function A(props) { body return returned; }`.
inline std::string component(std::string_view body, std::string_view returned = "<View/>",
                             std::string_view params = "") {
    return "function A(" + std::string(params) + ") {\n" + std::string(body) + "  return " +
           std::string(returned) + ";\n}\n";
}

inline const rules::ComponentAnalysis* analysis_of(const Fixture& fixture) {
    check(fixture.analyzed.components.size() == 1, "one component analysed");
    return fixture.analyzed.components.empty() ? nullptr : &fixture.analyzed.components[0];
}

inline lowering::LoweredComponentResult lower(const Fixture& fixture) {
    const rules::ComponentAnalysis* analysis = analysis_of(fixture);
    if (analysis == nullptr || analysis->item == nullptr) {
        return std::unexpected(lowering::NodeLoweringErrors{});
    }
    const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture.map,
                                                "ui/Counter.tsx"};
    return lowering::lower_component(context);
}

inline std::string strip_trailing_newline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// One-line rendering of a JSON value; objects keep the insertion order of the IR JSON writer.
inline std::string compact(const ir::JsonValue& value) {
    const auto& data = value.data();
    if (std::holds_alternative<std::monostate>(data)) {
        return "null";
    }
    if (const auto* boolean = std::get_if<bool>(&data)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&data)) {
        return std::to_string(*integer);
    }
    if (std::holds_alternative<double>(data) || std::holds_alternative<std::string>(data)) {
        return strip_trailing_newline(ir::to_text(value));
    }
    if (const auto* array = std::get_if<ir::JsonValue::Array>(&data)) {
        std::string out = "[";
        for (std::size_t i = 0; i < array->size(); ++i) {
            out += (i == 0 ? "" : ",") + compact((*array)[i]);
        }
        return out + "]";
    }
    if (const auto* object = std::get_if<ir::JsonValue::Object>(&data)) {
        std::string out = "{";
        for (std::size_t i = 0; i < object->size(); ++i) {
            out += (i == 0 ? "" : ",") + compact(ir::JsonValue::string((*object)[i].key)) + ":" +
                   compact((*object)[i].value);
        }
        return out + "}";
    }
    return "<json>";
}

inline std::string expr_text(const ir::Expr* expr) {
    return compact(ir::expr_to_json(expr));
}

inline std::string_view node_kind(const ir::Node& node) {
    return ir::node_kind_name(node.kind);
}

// A deterministic one-line-per-field walk of a node subtree, used for exact assertions.
inline void append_node(const ir::Node& node, std::string& out) {
    out += std::string(node_kind(node)) + " " + node.id;
    if (!node.automation_id.empty()) {
        out += " automation=" + node.automation_id;
    }
    if (node.key) {
        out += " key=" + expr_text(node.key.get());
    }
    for (const auto& [name, value] : node.props) {
        out += " prop " + name + "=" + expr_text(value.get());
    }
    for (const auto& [name, value] : node.style) {
        out += " style " + name + "=" + expr_text(value.get());
    }
    for (const auto& [name, value] : node.events) {
        out += " event " + name + "=" + value;
    }
    if (node.text) {
        out += " text=" + expr_text(node.text.get());
    }
    if (node.fallback) {
        out += " fallback[";
        append_node(*node.fallback, out);
        out += "]";
    }
    for (const ir::Node& child : node.children) {
        out += " child[";
        append_node(child, out);
        out += "]";
    }
}

inline std::string describe(const ir::Node& root) {
    std::string out;
    append_node(root, out);
    return out;
}

inline std::string describe_errors(const Fixture& fixture,
                                   const lowering::LoweredComponentResult& result) {
    std::string out;
    if (result) {
        return out;
    }
    for (const jsx::Diagnostic& diagnostic : result.error()) {
        out += std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) +
               ":" + std::to_string(diagnostic.span.start.column) + " | " +
               std::string(fixture.map.slice(diagnostic.span.span)) + " | " + diagnostic.message +
               "\n";
    }
    return out;
}

template<typename Value>
inline std::string first_code(const std::expected<Value, lowering::NodeLoweringErrors>& result) {
    if (result || result.error().empty()) {
        return {};
    }
    return std::string(result.error().front().code);
}

} // namespace lowering_nodes_test
