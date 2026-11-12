// SPDX-License-Identifier: MIT

// Shared helpers for the lowering_hooks_* tests: parse a component, run the analysis pass, lower
// its hooks and bindings, and describe the result.

#pragma once

#include "jsx_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/hook_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lowering_hooks_test {

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

// Parses and analyses `text`; a parser or analysis error fails the test, so lowering fixtures
// cannot pass or fail for a reason outside slice 6.
inline std::unique_ptr<Fixture> parse(std::string text) {
    auto fixture = std::make_unique<Fixture>(std::move(text));
    check(fixture->parsed.ok(), "fixture parses cleanly: " + codes(fixture->parsed));
    check(!fixture->analyzed.has_errors(),
          "fixture analyses cleanly: " +
              (fixture->analyzed.diagnostics.empty()
                   ? std::string()
                   : std::string(fixture->analyzed.diagnostics[0].message)));
    return fixture;
}

// Parses only; for fixtures that are expected to be rejected by the analysis pass.
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

// Lowers the first component of the fixture.
inline lowering::HookLoweringResult lower(const Fixture& fixture) {
    const rules::ComponentAnalysis* analysis = analysis_of(fixture);
    if (analysis == nullptr || analysis->item == nullptr) {
        return std::unexpected(lowering::HookLoweringErrors{});
    }
    const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture.map,
                                                "ui/Counter.tsx"};
    return lowering::lower_hooks(context);
}

inline std::string span_text(const ir::SourceSpan& span) {
    return span.file + ":" + std::to_string(span.line) + ":" + std::to_string(span.column);
}

inline std::string strip_trailing_newline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// One-line rendering of a JSON value, for readable expression assertions. Objects keep the
// insertion order of `to_json`, which is a pure function of the IR.
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

inline std::string params_text(const std::vector<ir::Param>& params) {
    std::string out;
    for (const ir::Param& param : params) {
        out += (out.empty() ? "" : ",") + param.name + ":" + param.type;
    }
    return "[" + out + "]";
}

inline std::string deps_text(const std::vector<std::string>& deps) {
    std::string out;
    for (const std::string& dep : deps) {
        out += (out.empty() ? "" : ",") + dep;
    }
    return "[" + out + "]";
}

inline std::string action_text(const ir::Action& action) {
    std::string out = action.kind == ir::ActionKind::Set ? "set " : "animSet ";
    out += action.target + " = " + expr_text(action.value.get());
    if (action.kind == ir::ActionKind::AnimSet) {
        out += action.animate ? " animate" : " animate=false";
        if (action.duration_ms.has_value()) {
            out += " duration=" + std::to_string(*action.duration_ms);
        }
        if (!action.easing.empty()) {
            out += " easing=" + action.easing;
        }
        if (action.spring.has_value()) {
            out += " spring=" + std::to_string(action.spring->stiffness) + "/" +
                   std::to_string(action.spring->damping) + "/" +
                   std::to_string(action.spring->mass);
        }
        if (!action.on_complete.empty()) {
            out += " onComplete=" + action.on_complete;
        }
    }
    return out;
}

// One line per field, in vector order: the determinism test compares the whole text.
inline std::string describe(const lowering::LoweredHooks& hooks) {
    std::string out = "component " + hooks.component_id + "\n";
    for (const ir::State& state : hooks.state) {
        out += "state " + state.id + " " + state.name + " " + state.type + " " +
               expr_text(state.initial.get()) + "\n";
    }
    for (const ir::Ref& ref : hooks.refs) {
        out += "ref " + ref.id + " " + ref.name + " target=" + ref.target + "\n";
    }
    for (const ir::AnimatedValue& value : hooks.animated_values) {
        out += "anim " + value.id + " " + value.name + " " + value.type + " " +
               expr_text(value.initial.get()) + "\n";
    }
    for (const ir::Handler& handler : hooks.handlers) {
        out += "handler " + handler.id + " params=" + params_text(handler.params) +
               " source=" + span_text(handler.source) + "\n";
        for (const ir::Action& action : handler.actions) {
            out += "  " + action_text(action) + "\n";
        }
    }
    for (const ir::Effect& effect : hooks.effects) {
        out += "effect " + effect.id + " deps=" + deps_text(effect.deps) +
               " handler=" + effect.handler + " cleanup=" + effect.cleanup +
               " source=" + span_text(effect.source) + "\n";
    }
    for (const ir::Binding& binding : hooks.bindings) {
        out += "binding " + binding.id + " " + binding.component + " " +
               std::string(ir::binding_kind_name(binding.kind)) + " region=" + binding.region +
               " params=" + params_text(binding.params) + " return=" + binding.return_type +
               " source=" + span_text(binding.source) + "\n";
    }
    for (const lowering::NamedRef& ref : hooks.ref_index) {
        out += "index ref " + ref.name + "=" + ref.id + "\n";
    }
    for (const lowering::NamedRef& value : hooks.animated_index) {
        out += "index anim " + value.name + "=" + value.id + "\n";
    }
    for (const lowering::NamedSetter& setter : hooks.setter_index) {
        out += "index setter " + setter.setter + "=" + setter.state_id + "\n";
    }
    for (const lowering::NamedHandler& handler : hooks.handler_index) {
        out += "index handler " + handler.name + "=" + handler.ref.id + " (" +
               (handler.ref.kind == lowering::HandlerRefKind::Binding ? "binding" : "inline") +
               ")\n";
    }
    out += "next handler " + std::to_string(hooks.next_handler_number) + "\n";
    return out;
}

// "<code> | line:col | source text of the span | message", like the parser and rules tests.
inline std::string describe_errors(const Fixture& fixture,
                                   const lowering::HookLoweringResult& result) {
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

inline std::string first_code_impl(const lowering::HookLoweringErrors& errors) {
    return errors.empty() ? std::string() : std::string(errors.front().code);
}

template<typename Value>
inline std::string first_code(const std::expected<Value, lowering::HookLoweringErrors>& result) {
    return result ? std::string() : first_code_impl(result.error());
}

} // namespace lowering_hooks_test
