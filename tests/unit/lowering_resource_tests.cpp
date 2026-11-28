// SPDX-License-Identifier: MIT

// Slice 3 of issue #656 (ADR-067, docs/specs/pending-ui-v1.md sections 3 and 6): `useResource` and
// `useDeferredValue` from TSX. The analysis half (every accepted form and diagnostic), the lowering
// half (the IR `resources` entry, its typed `data`, the `res::` references, the `deferred_value`
// behavior) and the validator half (TESSERA_SUSPENSE_NO_BOUNDARY and
// TESSERA_SUSPENSE_TOO_MANY_READS reported with the source position of the TSX that caused them).

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace services = tessera::compiler::services;

services::ServicesManifest make_manifest() {
    services::ManifestService api;
    api.id = "demo.api";
    api.name = "Api";
    api.cpp_type = "demo::Api";
    api.header = "demo/api.hpp";
    api.methods.push_back({"loadFeed", {{"query", "string"}}, "string"});
    api.methods.push_back({"countItems", {{"query", "string"}}, "int"});
    api.methods.push_back({"loadRoute", {{"query", "string"}}, "Route"});
    api.methods.push_back({"reset", {}, "void"});
    services::ServicesManifest manifest;
    manifest.services.push_back(std::move(api));
    return manifest;
}

struct ServiceFile {
    ServiceFile(std::string path, std::string text, const services::ServicesManifest* manifest)
        : file(std::move(path)),
          source(std::move(text)),
          parsed(jsx::parse_module(source)),
          map(source),
          analyzed(rules::analyze_module(*parsed.root, map, {}, nullptr, nullptr, manifest)) {}
    ServiceFile(const ServiceFile&) = delete;
    ServiceFile& operator=(const ServiceFile&) = delete;

    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{file, parsed.root.get(), &analyzed, &map};
    }
    [[nodiscard]] std::string first_code() const {
        return analyzed.diagnostics.empty() ? std::string()
                                            : std::string(analyzed.diagnostics.front().code);
    }

    std::string file;
    std::string source;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;
};

constexpr std::string_view kImports =
    "import { useState, useService, useResource, useTransition, useDeferredValue, Suspense } from "
    "\"tessera\";\n";

std::string component(std::string_view body, std::string_view root) {
    return std::string(kImports) + "export function Feed() {\n" + std::string(body) + "  return " +
           std::string(root) + ";\n}\n";
}

constexpr std::string_view kApi = "  const api = useService<Api>();\n"
                                  "  const [query, setQuery] = useState<string>(\"\");\n";

struct Lowered {
    std::unique_ptr<ServiceFile> file;
    std::optional<ir::IrModule> module;
    std::string codes; // validator codes, ';'-joined
    std::vector<ir::IrDiagnostic> diagnostics;
    std::string error;
};

Lowered lower(std::string text, const services::ServicesManifest& manifest) {
    Lowered out;
    out.file = std::make_unique<ServiceFile>("ui/Feed.tsx", std::move(text), &manifest);
    if (out.file->analyzed.has_errors()) {
        out.error = "analysis: " + out.file->first_code();
        return out;
    }
    const std::vector<lowering::ModuleSource> sources{out.file->module_source()};
    lowering::ModuleLoweringResult result = lowering::lower_project(sources, "feed");
    if (!result) {
        out.error = "lowering: " + std::string(result.error().front().code) + ": " +
                    result.error().front().message;
        return out;
    }
    ir::IrModule module;
    module.name = result->name;
    module.components = std::move(result->components);
    module.bindings = std::move(result->bindings);
    module.stylesheets = std::move(result->stylesheets);
    module.types = std::move(result->types);
    check(tessera::compiler::ids::assign_identity(module, "feed").has_value(), "identity");
    out.diagnostics = ir::validate(module);
    for (const auto& diagnostic : out.diagnostics) {
        out.codes += diagnostic.code + ";";
    }
    out.module = std::move(module);
    return out;
}

constexpr std::string_view kBoundaryRoot =
    "<Suspense fallback={<Text>Loading</Text>}><View><Text>{feed.data}</Text>"
    "<Text>{feed.status}</Text></View></Suspense>";

void test_a_bound_resource_lowers_typed() {
    const services::ServicesManifest manifest = make_manifest();
    const Lowered lowered = lower(
        component(std::string(kApi) +
                      "  const feed = useResource(api.loadFeed, { query }, { suspend: true });\n",
                  kBoundaryRoot),
        manifest);
    check(lowered.module.has_value(), "the component lowers: " + lowered.error);
    if (!lowered.module) {
        return;
    }
    check_eq(lowered.codes, "", "the lowered module validates");
    const ir::Component& feed = lowered.module->components.at(0);
    check(feed.resources.size() == 1, "one resource");
    if (feed.resources.empty()) {
        return;
    }
    const ir::Resource& resource = feed.resources[0];
    check(resource.id == "res::feed" && resource.service == "svc::api" &&
              resource.method == "loadFeed" && resource.suspend,
          "the entry names the binding, the service, the method and the suspending read");
    check_eq(resource.type, "string", "data is typed from the service method's return type");
    check(resource.key.kind == ir::ExprKind::Record && resource.key.fields.size() == 1 &&
              resource.key.fields[0].name == "query" && resource.key.fields[0].value &&
              resource.key.fields[0].value->id == "state::query",
          "the key is the anonymous record of the shorthand field");
    check(resource.source.file == "ui/Feed.tsx" && resource.source.line == 5,
          "the entry carries the source position of the call");
    const std::string json = ir::write_ir_json(*lowered.module);
    check(json.find("\"resources\"") != std::string::npos &&
              json.find("\"res::feed\"") != std::string::npos &&
              json.find("\"type\"") != std::string::npos &&
              json.find("res::feed.data") != std::string::npos &&
              json.find("res::feed.status") != std::string::npos,
          "the IR JSON carries the resource and the res:: references");
}

void test_scalar_key_bare_call_and_types() {
    const services::ServicesManifest manifest = make_manifest();
    const Lowered lowered =
        lower(component(std::string(kApi) + "  useResource(api.loadFeed, query);\n"
                                            "  const total = useResource(api.countItems, query);\n"
                                            "  const route = useResource(api.loadRoute, query);\n",
                        "<View><Text>{total.data}</Text><Text>{route.status}</Text></View>"),
              manifest);
    check(lowered.module.has_value(), "the component lowers: " + lowered.error);
    if (!lowered.module) {
        return;
    }
    check_eq(lowered.codes, "", "the module validates");
    const auto& resources = lowered.module->components.at(0).resources;
    check(resources.size() == 3, "three resources in declaration order");
    if (resources.size() != 3) {
        return;
    }
    check_eq(resources[0].id, "res::loadFeed_1", "a bare call is named res::<method>_<n> from 1");
    check(!resources[0].suspend && resources[0].key.kind == ir::ExprKind::Ref,
          "a scalar key is the expression itself and suspend defaults to false");
    check_eq(resources[1].type, "int", "an int method types data as int");
    check_eq(resources[2].type, "", "a record result has no scalar type: data stays untyped");
}

void test_no_boundary_has_a_source_position() {
    const services::ServicesManifest manifest = make_manifest();
    const Lowered lowered =
        lower(component(std::string(kApi) +
                            "  const feed = useResource(api.loadFeed, query, { suspend: true });\n",
                        "<Text>{feed.data}</Text>"),
              manifest);
    check(lowered.module.has_value(), "the component lowers: " + lowered.error);
    check(lowered.codes.find("TESSERA_SUSPENSE_NO_BOUNDARY") != std::string::npos,
          "a suspending read with no Suspense above it is reported: " + lowered.codes);
    for (const auto& diagnostic : lowered.diagnostics) {
        if (diagnostic.code == "TESSERA_SUSPENSE_NO_BOUNDARY") {
            check(diagnostic.span.file == "ui/Feed.tsx" && diagnostic.span.line == 5,
                  "the diagnostic points at the useResource call, line " +
                      std::to_string(diagnostic.span.line));
        }
    }
    // A non-suspending resource needs no boundary, and neither does reading only its status.
    const Lowered plain =
        lower(component(std::string(kApi) + "  const feed = useResource(api.loadFeed, query);\n",
                        "<Text>{feed.data}</Text>"),
              manifest);
    check_eq(plain.codes, "", "a resource without suspend never needs a boundary");
}

void test_too_many_reads_has_a_source_position() {
    const services::ServicesManifest manifest = make_manifest();
    std::string body = std::string(kApi);
    std::string root = "<Suspense fallback={<Text>Loading</Text>}><View>";
    for (int index = 0; index < 9; ++index) {
        const std::string name = "r" + std::to_string(index);
        body += "  const " + name + " = useResource(api.loadFeed, query, { suspend: true });\n";
        root += "<Text>{" + name + ".data}</Text>";
    }
    root += "</View></Suspense>";
    const Lowered lowered = lower(component(body, root), manifest);
    check(lowered.module.has_value(), "the component lowers: " + lowered.error);
    bool found = false;
    for (const auto& diagnostic : lowered.diagnostics) {
        if (diagnostic.code == "TESSERA_SUSPENSE_TOO_MANY_READS") {
            found = true;
            check(diagnostic.span.file == "ui/Feed.tsx" && diagnostic.span.line > 0,
                  "the limit is reported at the boundary's source position");
        }
    }
    check(found, "nine suspending reads under one boundary are reported: " + lowered.codes);
    // Eight are the limit and are accepted.
    std::string eight_body = std::string(kApi);
    std::string eight_root = "<Suspense fallback={<Text>Loading</Text>}><View>";
    for (int index = 0; index < 8; ++index) {
        const std::string name = "r" + std::to_string(index);
        eight_body +=
            "  const " + name + " = useResource(api.loadFeed, query, { suspend: true });\n";
        eight_root += "<Text>{" + name + ".data}</Text>";
    }
    eight_root += "</View></Suspense>";
    check_eq(lower(component(eight_body, eight_root), manifest).codes, "",
             "eight suspending reads are within the limit");
}

void expect_code(std::string_view name, std::string_view body, std::string_view root,
                 std::string_view code, const services::ServicesManifest& manifest) {
    const ServiceFile file("ui/Feed.tsx", component(body, root), &manifest);
    check(file.first_code() == code,
          std::string(name) + ": expected " + std::string(code) + ", got " + file.first_code());
}

void test_resource_diagnostics() {
    const services::ServicesManifest manifest = make_manifest();
    const std::string_view root = "<View />";
    expect_code("a URL instead of a service method",
                std::string(kApi) + "  const f = useResource(\"/feed\", query);\n", root,
                "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("a function instead of a service method",
                std::string(kApi) + "  const f = useResource(() => 1, query);\n", root,
                "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("a name that is not a service binding",
                std::string(kApi) + "  const f = useResource(query.load, query);\n", root,
                "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("a method the manifest does not list",
                std::string(kApi) + "  const f = useResource(api.missing, query);\n", root,
                "TESSERA_REACT_SERVICE_METHOD", manifest);
    expect_code("an impure key",
                std::string(kApi) + "  const f = useResource(api.loadFeed, { q: query.trim() });\n",
                root, "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("an unknown option",
                std::string(kApi) +
                    "  const f = useResource(api.loadFeed, query, { retry: true });\n",
                root, "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("a non-literal suspend",
                std::string(kApi) +
                    "  const f = useResource(api.loadFeed, query, { suspend: query });\n",
                root, "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("too few arguments", std::string(kApi) + "  const f = useResource(api.loadFeed);\n",
                root, "TESSERA_REACT_HOOK_ARG", manifest);
    expect_code("a destructured binding",
                std::string(kApi) + "  const [f] = useResource(api.loadFeed, query);\n", root,
                "TESSERA_REACT_HOOK_FORM", manifest);
    expect_code("a service declared after the resource",
                "  const [query, setQuery] = useState<string>(\"\");\n"
                "  const f = useResource(api.loadFeed, query);\n"
                "  const api = useService<Api>();\n",
                root, "TESSERA_REACT_HOOK_ORDER", manifest);
    expect_code("a call in a condition",
                std::string(kApi) + "  const f = query ? useResource(api.loadFeed, query) : 0;\n",
                root, "TESSERA_REACT_HOOK_ORDER", manifest);
}

void test_deferred_value_lowering() {
    const services::ServicesManifest manifest = make_manifest();
    const Lowered lowered =
        lower(component("  const [mode, setMode] = useState<\"fast\" | \"slow\">(\"fast\");\n"
                        "  const [count, setCount] = useState<int>(0);\n"
                        "  const settled = useDeferredValue(mode);\n"
                        "  const total = useDeferredValue(count);\n",
                        "<View><Text>{settled}</Text><Text>{total}</Text></View>"),
              manifest);
    check(lowered.module.has_value(), "the component lowers: " + lowered.error);
    if (!lowered.module) {
        return;
    }
    check_eq(lowered.codes, "", "the module validates");
    const auto& behaviors = lowered.module->components.at(0).behaviors;
    check(behaviors.size() == 2, "two deferred values");
    if (behaviors.size() != 2) {
        return;
    }
    check(behaviors[0].id == "beh::settled" && behaviors[0].hook == "useDeferredValue" &&
              behaviors[0].behavior == "deferred_value" && behaviors[0].targets.empty(),
          "the entry names the hook and the engine behavior, with no target");
    check(behaviors[0].args.size() == 1 && behaviors[0].args[0].name == "value" &&
              behaviors[0].args[0].expr.id == "state::mode",
          "its one arg is the state it defers");
    check(behaviors[0].outputs.size() == 1 && behaviors[0].outputs[0].name == "value" &&
              behaviors[0].outputs[0].type == "string" && behaviors[1].outputs[0].type == "int",
          "the output has the type of the deferred state");
    const std::string json = ir::write_ir_json(*lowered.module);
    check(json.find("beh::settled.value") != std::string::npos,
          "a read of the deferred name lowers to beh::<name>.value");
}

void test_deferred_value_rejections() {
    const services::ServicesManifest manifest = make_manifest();
    const auto expect_error = [&](std::string_view name, std::string_view body,
                                  std::string_view code) {
        const Lowered lowered = lower(component(body, "<Text>{d}</Text>"), manifest);
        check(lowered.error.find(code) != std::string::npos,
              std::string(name) + ": expected " + std::string(code) + ", got " + lowered.error);
    };
    expect_error("a plain string",
                 "  const [q, setQ] = useState<string>(\"\");\n"
                 "  const d = useDeferredValue(q);\n",
                 "TESSERA_IR_UNSUPPORTED");
    expect_error("a list",
                 "  const [items, setItems] = useState<string[]>([]);\n"
                 "  const d = useDeferredValue(items);\n",
                 "TESSERA_IR_UNSUPPORTED");
    expect_error("an inferred plain string",
                 "  const [q, setQ] = useState(\"x\");\n"
                 "  const d = useDeferredValue(q);\n",
                 "TESSERA_IR_UNSUPPORTED");
    expect_error("a non-state argument", "  const d = useDeferredValue(1 + 1);\n",
                 "TESSERA_REACT_HOOK_FORM");
    expect_error("a prop-less name", "  const d = useDeferredValue(missing);\n",
                 "TESSERA_REACT_HOOK_ARG");
}

} // namespace

int main() {
    test_a_bound_resource_lowers_typed();
    test_scalar_key_bare_call_and_types();
    test_no_boundary_has_a_source_position();
    test_too_many_reads_has_a_source_position();
    test_resource_diagnostics();
    test_deferred_value_lowering();
    test_deferred_value_rejections();
    if (failures == 0) {
        std::cout << "lowering resource tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
