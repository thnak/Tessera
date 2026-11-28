// SPDX-License-Identifier: MIT
//
// Build-time tool of the `codegen_observable_tsx` runtime test (ADR-050 open question 6): it
// compiles a TSX component that reads two service observables in render, through the real front end
// (parse, analyse against a services manifest with `observables`, lower, generate the ADR-040
// base), and writes `Header.g.hpp` into the directory given as argv[1]. The runtime test compiles
// that header against a hand-written `demo::Navigator` and drives the observables. Exit 1 with the
// first diagnostic on any front-end failure.

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace codegen = tessera::compiler::codegen;
namespace ir = tessera::compiler::ir;
namespace jsx = tessera::compiler::jsx;
namespace lowering = tessera::compiler::lowering;
namespace rules = tessera::compiler::rules;
namespace services = tessera::compiler::services;

constexpr const char* kSource =
    "import { useService, useObservable } from \"tessera\";\n"
    "export function Header() {\n"
    "  const nav = useService<Navigator>();\n"
    "  const title = useObservable(nav.title);\n"
    "  const unread = useObservable(nav.unread);\n"
    "  return <View><Text>{title}</Text><Text>{`${unread} new`}</Text></View>;\n"
    "}\n";

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: codegen_observable_tsx_emit <out-dir>\n";
        return 2;
    }
    services::ManifestService nav;
    nav.id = "demo.nav";
    nav.name = "Navigator";
    nav.cpp_type = "demo::Navigator";
    nav.header = "demo/observable_nav.hpp";
    nav.observables.push_back({"title", "string"});
    nav.observables.push_back({"unread", "int"});
    services::ServicesManifest manifest;
    manifest.services.push_back(std::move(nav));

    const std::string text = kSource;
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    const jsx::SourceMap map(text);
    if (!parsed.ok()) {
        std::cerr << "the fixture does not parse\n";
        return 1;
    }
    const rules::AnalyzedModule analyzed =
        rules::analyze_module(*parsed.root, map, {}, nullptr, nullptr, &manifest);
    if (analyzed.has_errors()) {
        std::cerr << "analysis: " << analyzed.diagnostics.front().code << ": "
                  << analyzed.diagnostics.front().message << "\n";
        return 1;
    }
    const std::vector<lowering::ModuleSource> sources{
        lowering::ModuleSource{"ui/Header.tsx", parsed.root.get(), &analyzed, &map}};
    lowering::ModuleLoweringResult lowered = lowering::lower_project(sources, "demo");
    if (!lowered) {
        std::cerr << "lowering: " << lowered.error().front().code << ": "
                  << lowered.error().front().message << "\n";
        return 1;
    }
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);
    module.types = std::move(lowered->types);
    const codegen::CodegenResult generated = codegen::generate(module, {}, std::nullopt);
    if (!generated) {
        std::cerr << "codegen: " << generated.error().front() << "\n";
        return 1;
    }
    for (const codegen::GeneratedFile& file : *generated) {
        std::ofstream out(std::filesystem::path(argv[1]) / file.path, std::ios::binary);
        out << file.text;
    }
    return 0;
}
