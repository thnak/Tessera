// SPDX-License-Identifier: MIT
//
// `services.d.ts` generation (docs/specs/services-manifest-v1.md section 3 item 4): the type
// mapping and a golden over a two-service manifest.

#include "tessera/compiler/services/manifest.hpp"
#include "tessera/compiler/services/typings.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace svc = tessera::compiler::services;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

void type_mapping() {
    check(svc::manifest_type_to_ts("bool") == "boolean", "bool");
    check(svc::manifest_type_to_ts("int") == "number", "int");
    check(svc::manifest_type_to_ts("float") == "number", "float");
    check(svc::manifest_type_to_ts("color") == "string", "color");
    check(svc::manifest_type_to_ts("void") == "void", "void");
    check(svc::manifest_type_to_ts("Route") == "Route", "a record keeps its name");
    check(svc::manifest_type_to_ts("optional<int>") == "number | null", "optional");
    check(svc::manifest_type_to_ts("list<string>") == "string[]", "list");
    check(svc::manifest_type_to_ts("list<optional<int>>") == "(number | null)[]",
          "a list of optionals is parenthesised");
}

constexpr std::string_view kManifest = R"({
  "version": 1,
  "services": [
    { "id": "demo.nav", "name": "Navigator",
      "cpp": { "type": "demo::Navigator", "header": "demo/navigator.hpp" },
      "methods": [
        { "name": "push", "params": [{ "name": "route", "type": "string" }], "returns": "void" },
        { "name": "move", "params": [{ "name": "a", "type": "int" }, { "name": "b", "type": "optional<int>" }],
          "returns": "bool" }
      ] },
    { "id": "demo.clock", "name": "Clock", "cpp": { "type": "demo::Clock", "header": "demo/clock.hpp" },
      "methods": [] }
  ]
})";

constexpr std::string_view kGolden =
    "// Generated from ui/services.json by tessera_codegen. Do not edit.\n"
    "// Types the services TSX may name in useService<T>() and the methods it may call.\n"
    "\n"
    "export interface Navigator {\n"
    "  push(route: string): void;\n"
    "  move(a: number, b: number | null): boolean;\n"
    "}\n"
    "\n"
    "export interface Clock {\n"
    "}\n"
    "\n"
    "export declare function useService<T extends Navigator>(): Navigator;\n"
    "export declare function useService<T extends Clock>(): Clock;\n";

void golden_output() {
    const auto parsed = svc::parse_services_manifest(kManifest);
    check(parsed.has_value(), "the manifest parses");
    if (!parsed) {
        return;
    }
    const std::string text = svc::write_services_typings(*parsed);
    check(text == kGolden, "the typings match the golden");
    if (text != kGolden) {
        std::cerr << text;
    }
    check(svc::write_services_typings(*parsed) == text, "generation is deterministic");
}

} // namespace

int main() {
    type_mapping();
    golden_output();
    return failures == 0 ? 0 : 1;
}
