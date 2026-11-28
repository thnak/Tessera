// SPDX-License-Identifier: MIT

// The TSX fixture of the #656 end-to-end tests (docs/specs/pending-ui-v1.md): one component that
// reads a typed suspending resource, marks a setter with `startTransition`, shows `isPending` and a
// `useDeferredValue`. The emit tool compiles it to `Feed.g.hpp` at build time and the parity test
// lowers the same text again for the evaluator, so both halves come from one source.

#pragma once

#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::pending_tsx {

namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace services = tessera::compiler::services;

inline constexpr const char* kSource =
    "import { useState, useService, useResource, useTransition, useDeferredValue, Suspense } "
    "from \"tessera\";\n"
    "export function Feed() {\n"
    "  const nav = useService<Navigator>();\n"
    "  const [mode, setMode] = useState<\"fast\" | \"slow\">(\"fast\");\n"
    "  const [isPending, startTransition] = useTransition();\n"
    "  const settled = useDeferredValue(mode);\n"
    "  const feed = useResource(nav.loadFeed, { mode }, { suspend: true });\n"
    "  return (\n"
    "    <View>\n"
    "      <Suspense fallback={<Text>Loading</Text>}>\n"
    "        <View><Text>{feed.data}</Text></View>\n"
    "      </Suspense>\n"
    "      <Button onClick={() => startTransition(() => setMode(\"slow\"))}>\n"
    "        <Text>Slow</Text>\n"
    "      </Button>\n"
    "      {isPending && <Text>Updating</Text>}\n"
    "      <Text>{settled}</Text>\n"
    "      <Text>{feed.status}</Text>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

// `Navigator` is the services fixture's C++ interface (`demo::Navigator`); the generated component
// never calls `loadFeed`, the host binds a view per resource, so only the manifest needs the
// method.
inline services::ServicesManifest make_manifest() {
    services::ManifestService nav;
    nav.id = "demo.nav";
    nav.name = "Navigator";
    nav.cpp_type = "demo::Navigator";
    nav.header = "demo/services.hpp";
    nav.methods.push_back({"loadFeed", {{"mode", "string"}}, "string"});
    services::ServicesManifest manifest;
    manifest.services.push_back(std::move(nav));
    return manifest;
}

// Compiles `source` through the real front end. Nullopt with `error` set on any diagnostic.
inline std::optional<ir::IrModule> compile(const std::string& source, std::string& error) {
    const services::ServicesManifest manifest = make_manifest();
    const jsx::ModuleParseResult parsed = jsx::parse_module(source);
    const jsx::SourceMap map(source);
    if (!parsed.ok()) {
        error = "the fixture does not parse";
        return std::nullopt;
    }
    const rules::AnalyzedModule analyzed =
        rules::analyze_module(*parsed.root, map, {}, nullptr, nullptr, &manifest);
    if (analyzed.has_errors()) {
        error = std::string("analysis: ") + std::string(analyzed.diagnostics.front().code) + ": " +
                analyzed.diagnostics.front().message;
        return std::nullopt;
    }
    const std::vector<lowering::ModuleSource> sources{
        lowering::ModuleSource{"ui/Feed.tsx", parsed.root.get(), &analyzed, &map}};
    lowering::ModuleLoweringResult lowered = lowering::lower_project(sources, "pending_tsx");
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
    if (!tessera::compiler::ids::assign_identity(module, "pending_tsx").has_value()) {
        error = "identity assignment failed";
        return std::nullopt;
    }
    return module;
}

} // namespace tessera::test::pending_tsx
