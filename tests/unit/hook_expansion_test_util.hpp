// SPDX-License-Identifier: MIT

// Shared helpers for the custom-hook expansion tests (spike #659, ADR-065): run the front end with
// the experimental switch on, and compare a component that uses a hook with the hand-expanded
// component it should be equivalent to.

#pragma once

#include "jsx_test_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/jsx/hook_expansion.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hook_expansion_test {

using namespace jsx_test;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

// One file through the whole front end: parse, expand, analyse, lower. The members are ordered so
// each stage outlives the views the next one takes of it.
struct Pipeline {
    explicit Pipeline(std::string text, bool custom_hooks)
        : source(std::move(text)),
          options{.custom_hooks = custom_hooks},
          parsed(jsx::parse_module(source, jsx::Limits{}, options)),
          map(source) {
        if (parsed.root != nullptr) {
            sink = std::make_unique<jsx::DiagnosticSink>(map, jsx::Limits{});
            metrics = jsx::expand_custom_hooks(*parsed.root, map, options, *sink);
            analyzed = rules::analyze_module(*parsed.root, map, jsx::Limits{}, nullptr,
                                             &metrics.call_sites);
        }
    }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] bool ok() const {
        return parsed.root != nullptr && !parsed.has_errors() && !analyzed.has_errors() &&
               (sink == nullptr || !sink->has_errors());
    }

    [[nodiscard]] lowering::ModuleLoweringResult lower() const {
        if (!ok()) {
            return std::unexpected(lowering::NodeLoweringErrors{});
        }
        const lowering::ModuleSource module_source{"ui/Counter.tsx", parsed.root.get(), &analyzed,
                                                   &map};
        return lowering::lower_project(std::span<const lowering::ModuleSource>(&module_source, 1),
                                       "counter");
    }

    std::string source;
    jsx::FrontEndOptions options;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    std::unique_ptr<jsx::DiagnosticSink> sink;
    jsx::ExpansionMetrics metrics;
    rules::AnalyzedModule analyzed;
};

// The first diagnostic of a pipeline, formatted like the other front-end tests.
inline std::string first_diagnostic(const Pipeline& pipeline) {
    if (!pipeline.parsed.diagnostics.empty()) {
        const jsx::Diagnostic& diagnostic = pipeline.parsed.diagnostics.front();
        return std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) +
               ":" + std::to_string(diagnostic.span.start.column) + " | " + diagnostic.message;
    }
    if (pipeline.sink != nullptr && !pipeline.sink->diagnostics().empty()) {
        const jsx::Diagnostic& diagnostic = pipeline.sink->diagnostics().front();
        return std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) +
               ":" + std::to_string(diagnostic.span.start.column) + " | " + diagnostic.message;
    }
    if (!pipeline.analyzed.diagnostics.empty()) {
        const jsx::Diagnostic& diagnostic = pipeline.analyzed.diagnostics.front();
        return std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) +
               ":" + std::to_string(diagnostic.span.start.column) + " | " + diagnostic.message;
    }
    return {};
}

// The IR carries a `source` object per component, handler, effect, ref, environment and node: the
// byte range in the file the construct came from. A hook's statements live in the hook declaration
// and the hand-expanded ones live in the component, so those offsets differ by construction and are
// not part of "the same IR". Every other field is compared.
//
// The comparison goes through the JSON model rather than the text, so a member is dropped by name
// and no string surgery can corrupt the document.
inline ir::JsonValue without_sources(const ir::JsonValue& value) {
    if (const auto* object = value.as_object(); object != nullptr) {
        ir::JsonValue kept = ir::JsonValue::object();
        for (const ir::JsonMember& member : *object) {
            if (member.key == "source") {
                continue;
            }
            kept.set(member.key, without_sources(member.value));
        }
        return kept;
    }
    if (const auto* array = value.as_array(); array != nullptr) {
        ir::JsonValue kept = ir::JsonValue::array();
        for (const ir::JsonValue& element : *array) {
            kept.push(without_sources(element));
        }
        return kept;
    }
    return value;
}

// The canonical IR text of `component` with every `source` member removed.
inline std::string component_ir_without_sources(const ir::Component& component) {
    std::string text = ir::to_text(without_sources(ir::to_json(component)));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// The canonical IR JSON of the first component, one line strip.
inline std::string component_ir(const lowering::LoweredModule& module) {
    if (module.components.empty()) {
        return "<no component>";
    }
    return component_ir_without_sources(module.components.front());
}

// The id mapping of the differential test: the expansion names every name it introduces
// `<hook>_<n>__<name>` (spec section 4 step 3), and the hand-expanded component uses the natural
// name. Each pair maps one expansion prefix to the suffix the hand-written component uses, so the
// comparison is about the shape of the component and not about the spelling the expansion chose.
//
// A pair rather than a bare prefix because two calls of one hook must stay distinct: the first
// call's `useFlag_1__on` is `on` and the second's `useFlag_2__on` is `onB`. Longest prefix first,
// so a shorter prefix cannot leave a stray ordinal behind.
using Rename = std::pair<std::string, std::string>;

inline std::string strip_expansion_prefixes(std::string text, const std::vector<Rename>& renames) {
    std::vector<Rename> ordered = renames;
    std::sort(ordered.begin(), ordered.end(),
              [](const Rename& a, const Rename& b) { return a.first.size() > b.first.size(); });
    for (const Rename& rename : ordered) {
        std::size_t at = text.find(rename.first);
        while (at != std::string::npos) {
            text.replace(at, rename.first.size(), rename.second);
            at = text.find(rename.first, at + rename.second.size());
        }
    }
    return text;
}

// The member names the generated base will require, in IR order: every `bind::` id of the module.
inline std::vector<std::string> bind_ids(const lowering::LoweredModule& module) {
    std::vector<std::string> ids;
    for (const ir::Binding& binding : module.bindings) {
        ids.push_back(binding.id);
    }
    return ids;
}

// The `state::`, `ref::`, `anim::` and `env::` ids of the first component, in IR order.
inline std::vector<std::string> declared_ids(const lowering::LoweredModule& module) {
    std::vector<std::string> ids;
    if (module.components.empty()) {
        return ids;
    }
    const ir::Component& component = module.components.front();
    for (const ir::State& state : component.state) {
        ids.push_back(state.id);
    }
    for (const ir::Ref& ref : component.refs) {
        ids.push_back(ref.id);
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        ids.push_back(value.id);
    }
    for (const ir::Environment& env : component.environment) {
        ids.push_back(env.id);
    }
    return ids;
}

} // namespace hook_expansion_test
