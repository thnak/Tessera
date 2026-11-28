// SPDX-License-Identifier: MIT

// The TSX fixture of the #1055 end-to-end test: a callee whose `useState` initializer reads a prop,
// and a parent that passes a non-default value. The emit tool compiles it to `Counter.g.hpp` and
// `Host.g.hpp` at build time, so the test observes what the generated C++ really does.

#pragma once

#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::state_init {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;

inline constexpr const char* kCounter =
    "import { useState } from \"tessera\";\n"
    "export type CounterProps = { start?: int; label?: string };\n"
    "export function Counter({ start = 3, label = \"n\" }: CounterProps) {\n"
    "  const [count, setCount] = useState<int>(start);\n"
    "  const [title, setTitle] = useState<string>(label);\n"
    "  return (\n"
    "    <View>\n"
    "      <Text>{count}</Text>\n"
    "      <Text>{title}</Text>\n"
    "      <Button onClick={() => setCount(count + 1)}><Text>inc</Text></Button>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

inline constexpr const char* kHost = "import { Counter } from \"./Counter\";\n"
                                     "export function Host() {\n"
                                     "  return (\n"
                                     "    <View>\n"
                                     "      <Counter start={7} label=\"seven\" />\n"
                                     "      <Counter />\n"
                                     "    </View>\n"
                                     "  );\n"
                                     "}\n";

// Compiles both sources through the real front end. Nullopt with `error` set on any diagnostic.
inline std::optional<ir::IrModule> compile(std::string& error) {
    const jsx::ModuleParseResult counter = jsx::parse_module(kCounter);
    const jsx::ModuleParseResult host = jsx::parse_module(kHost);
    const jsx::SourceMap counter_map(kCounter);
    const jsx::SourceMap host_map(kHost);
    if (!counter.ok() || !host.ok()) {
        error = "the fixture does not parse";
        return std::nullopt;
    }
    const rules::AnalyzedModule counter_analyzed =
        rules::analyze_module(*counter.root, counter_map, {}, nullptr, nullptr, nullptr);
    const rules::AnalyzedModule host_analyzed =
        rules::analyze_module(*host.root, host_map, {}, nullptr, nullptr, nullptr);
    for (const rules::AnalyzedModule* analyzed : {&counter_analyzed, &host_analyzed}) {
        if (analyzed->has_errors()) {
            error = std::string("analysis: ") + std::string(analyzed->diagnostics.front().code) +
                    ": " + analyzed->diagnostics.front().message;
            return std::nullopt;
        }
    }
    const std::vector<lowering::ModuleSource> sources{
        lowering::ModuleSource{"ui/Counter.tsx", counter.root.get(), &counter_analyzed,
                               &counter_map},
        lowering::ModuleSource{"ui/Host.tsx", host.root.get(), &host_analyzed, &host_map}};
    lowering::ModuleLoweringResult lowered = lowering::lower_project(sources, "state_init");
    if (!lowered) {
        error = "lowering: " + std::string(lowered.error().front().code) + ": " +
                lowered.error().front().message;
        return std::nullopt;
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);
    module.types = std::move(lowered->types);
    if (!tessera::compiler::ids::assign_identity(module, "state_init").has_value()) {
        error = "identity assignment failed";
        return std::nullopt;
    }
    return module;
}

} // namespace tessera::test::state_init
