// SPDX-License-Identifier: MIT
//
// Build-time tool of the `codegen_services_tsx` runtime test (ADR-050 section 3.1, issue #177): it
// compiles a TSX component that calls a service, through the real front end (parse, analyse against
// a services manifest, lower, generate the ADR-040 base), and writes `Inbox.g.hpp` into the
// directory given as argv[1]. The runtime test then compiles that header against a hand-written
// `demo::Navigator`, which is the only way to prove the generated `nav().push(...)` is valid C++
// and does what the TSX said. Exit 1 with the first diagnostic on any front-end failure.

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
    "import { useState, useService } from \"tessera\";\n"
    "export function Inbox() {\n"
    "  const nav = useService<Navigator>();\n"
    "  const [count, setCount] = useState<int>(0);\n"
    "  return <View onClick={() => { nav.push(\"/compose\"); setCount(count + 1); }} />;\n"
    "}\n";

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: codegen_services_tsx_emit <out-dir>\n";
        return 2;
    }
    services::ManifestService nav;
    nav.id = "demo.nav";
    nav.name = "Navigator";
    nav.cpp_type = "demo::Navigator";
    nav.header = "demo/services.hpp";
    nav.methods.push_back({"push", {{"route", "string"}}, "void"});
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
        lowering::ModuleSource{"ui/Inbox.tsx", parsed.root.get(), &analyzed, &map}};
    lowering::ModuleLoweringResult lowered = lowering::lower_project(sources, "demo");
    if (!lowered) {
        std::cerr << "lowering: " << lowered.error().front().code << ": "
                  << lowered.error().front().message << "\n";
        return 1;
    }
    // The IR module a driver assembles from the lowered one (regenerate.cpp does the same).
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
