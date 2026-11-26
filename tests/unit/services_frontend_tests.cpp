// SPDX-License-Identifier: MIT
//
// `useService<T>()` from TSX (ADR-050 section 3.1, docs/specs/services-manifest-v1.md section 3,
// issue #177): the analysis half (what is accepted and every diagnostic), the lowering half (the IR
// `services[]` entry and the `callService` action) and the evaluator half (the call reaches a host
// sink with its evaluated arguments). The generated C++ is pinned by `codegen_services_tsx`.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace eval = tessera::compiler::eval;
namespace services = tessera::compiler::services;

services::ServicesManifest make_manifest() {
    services::ManifestService nav;
    nav.id = "demo.nav";
    nav.name = "Navigator";
    nav.cpp_type = "demo::Navigator";
    nav.header = "demo/navigator.hpp";
    nav.methods.push_back({"push", {{"route", "string"}}, "void"});
    nav.methods.push_back({"back", {}, "void"});
    nav.methods.push_back({"reset", {{"depth", "int"}, {"animated", "bool"}}, "void"});
    services::ServicesManifest manifest;
    manifest.services.push_back(std::move(nav));
    return manifest;
}

// A file analysed against a manifest, which `FileFixture` (no manifest) cannot express.
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

// One component whose body is `body` and whose root is `root`.
std::string component(std::string_view body, std::string_view root) {
    return "import { useState, useService } from \"tessera\";\n"
           "export function Inbox({ route }: { route: string }) {\n" +
           std::string(body) + "  return " + std::string(root) + ";\n}\n";
}

constexpr std::string_view kHandlerRoot =
    "<View onClick={() => { nav.push(route); setCount(count + 1); }} />";
constexpr std::string_view kNavBody = "  const nav = useService<Navigator>();\n"
                                      "  const [count, setCount] = useState<int>(0);\n";

void test_an_accepted_use_is_recorded() {
    const services::ServicesManifest manifest = make_manifest();
    const ServiceFile file("ui/Inbox.tsx", component(kNavBody, kHandlerRoot), &manifest);
    check(file.parsed.ok(), "the fixture parses: " + codes(file.parsed));
    check(!file.analyzed.has_errors(), "an accepted use analyses cleanly: " + file.first_code());
    check(!file.analyzed.components.empty() &&
              file.analyzed.components.front().service_uses.size() == 1,
          "one service use is recorded");
    const auto& uses = file.analyzed.components.front().service_uses;
    check(!uses.empty() && uses.front().name == "nav" && uses.front().service == "Navigator" &&
              uses.front().decl != nullptr && uses.front().decl->id == "demo.nav",
          "the use names the binding, the interface and the manifest entry");
}

void test_lowering_produces_the_requirement_and_the_call() {
    const services::ServicesManifest manifest = make_manifest();
    auto file =
        std::make_unique<ServiceFile>("ui/Inbox.tsx", component(kNavBody, kHandlerRoot), &manifest);
    check(!file->analyzed.has_errors(), "the fixture analyses cleanly: " + file->first_code());
    const std::vector<lowering::ModuleSource> sources{file->module_source()};
    const lowering::ModuleLoweringResult result = lowering::lower_project(sources, "inbox");
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* inbox = find_component(*result, "Inbox");
    check(inbox != nullptr, "the component lowers");
    if (inbox == nullptr) {
        return;
    }
    check(inbox->services.size() == 1 && inbox->services[0].id == "svc::nav" &&
              inbox->services[0].name == "nav" && inbox->services[0].type == "demo::Navigator" &&
              inbox->services[0].header == "demo/navigator.hpp" &&
              inbox->services[0].registry_id == "demo.nav",
          "the requirement carries the manifest's C++ type, header and id");
    check(inbox->handlers.size() == 1 && inbox->handlers[0].actions.size() == 2,
          "the handler has the service call and the setter");
    if (inbox->handlers.empty() || inbox->handlers[0].actions.size() != 2) {
        return;
    }
    const ir::Action& call = inbox->handlers[0].actions[0];
    check(call.kind == ir::ActionKind::CallService && call.target == "svc::nav" &&
              call.method == "push" && call.args.size() == 1 && !call.value,
          "the call is a callService action with its method and one argument");
    check(call.args.size() == 1 && call.args[0] && call.args[0]->kind == ir::ExprKind::Ref &&
              call.args[0]->id == "prop::route",
          "the argument is the prop reference");
    check(inbox->handlers[0].actions[1].kind == ir::ActionKind::Set,
          "the setter after it is an ordinary set");
    const std::string json = describe(*result);
    check(json.find("callService") != std::string::npos &&
              json.find("registryId") != std::string::npos,
          "the IR JSON names the action kind and the registry id");
}

struct Recorded {
    std::string target;
    std::string method;
    std::vector<std::string> args;
};

void on_service_call(void* context, const std::string& target, const std::string& method,
                     const std::vector<eval::Value>& args) {
    Recorded& recorded = *static_cast<Recorded*>(context);
    recorded.target = target;
    recorded.method = method;
    for (const eval::Value& arg : args) {
        recorded.args.push_back(arg.as_string() != nullptr ? *arg.as_string() : "<not a string>");
    }
}

void test_the_evaluator_reaches_the_sink_with_evaluated_arguments() {
    const services::ServicesManifest manifest = make_manifest();
    auto file =
        std::make_unique<ServiceFile>("ui/Inbox.tsx", component(kNavBody, kHandlerRoot), &manifest);
    const std::vector<lowering::ModuleSource> sources{file->module_source()};
    const lowering::ModuleLoweringResult result = lowering::lower_project(sources, "inbox");
    const ir::Component* inbox = result ? find_component(*result, "Inbox") : nullptr;
    check(inbox != nullptr && !inbox->handlers.empty(), "the handler lowers");
    if (inbox == nullptr || inbox->handlers.empty()) {
        return;
    }
    eval::Evaluator evaluator;
    Recorded recorded;
    evaluator.set_service_call_sink(&on_service_call, &recorded);
    eval::Scope scope{{"prop::route", eval::Value::of_string("/compose")},
                      {"state::count", eval::Value::of_int(0)}};
    const auto ran = evaluator.run(inbox->handlers[0].actions, scope);
    check(ran.has_value(), "the handler runs");
    check(recorded.target == "svc::nav" && recorded.method == "push" && recorded.args.size() == 1 &&
              recorded.args[0] == "/compose",
          "the sink receives the target, the method and the evaluated argument");
    const auto count = scope.find("state::count");
    check(count != scope.end() && count->second.as_int() != nullptr && *count->second.as_int() == 1,
          "the setter after the call still ran");
    // Without a sink the call is skipped, never an error.
    eval::Evaluator bare;
    eval::Scope other{{"prop::route", eval::Value::of_string("/x")},
                      {"state::count", eval::Value::of_int(0)}};
    check(bare.run(inbox->handlers[0].actions, other).has_value(),
          "a call with no sink installed is skipped");
}

void expect_code(std::string_view name, std::string_view body, std::string_view root,
                 std::string_view code, const services::ServicesManifest* manifest) {
    const ServiceFile file("ui/Inbox.tsx", component(body, root), manifest);
    check(file.first_code() == code,
          std::string(name) + ": expected " + std::string(code) + ", got " + file.first_code());
}

void test_every_diagnostic_has_its_code() {
    const services::ServicesManifest manifest = make_manifest();
    const std::string_view empty_root = "<View />";
    expect_code("unknown interface", "  const nav = useService<Settings>();\n", empty_root,
                "TESSERA_REACT_SERVICE_UNKNOWN", &manifest);
    expect_code("no manifest at all", kNavBody, empty_root, "TESSERA_REACT_SERVICE_UNKNOWN",
                nullptr);
    expect_code("no type argument", "  const nav = useService();\n", empty_root,
                "TESSERA_REACT_HOOK_FORM", &manifest);
    expect_code("a destructured binding", "  const [nav] = useService<Navigator>();\n", empty_root,
                "TESSERA_REACT_HOOK_FORM", &manifest);
    expect_code("a method the manifest does not list", kNavBody,
                "<View onClick={() => nav.pop()} />", "TESSERA_REACT_SERVICE_METHOD", &manifest);
    expect_code("too few arguments", kNavBody, "<View onClick={() => nav.push()} />",
                "TESSERA_REACT_SERVICE_METHOD", &manifest);
    expect_code("too many arguments", kNavBody, "<View onClick={() => nav.back(1)} />",
                "TESSERA_REACT_SERVICE_METHOD", &manifest);
    expect_code("a string where the manifest says int", kNavBody,
                "<View onClick={() => nav.reset(\"x\", true)} />", "TESSERA_REACT_SERVICE_METHOD",
                &manifest);
    expect_code("a call while rendering", std::string(kNavBody) + "  const here = nav.back();\n",
                empty_root, "TESSERA_REACT_SERVICE_CALL_IN_RENDER", &manifest);
    expect_code("the service as a value", std::string(kNavBody) + "  const alias = nav;\n",
                empty_root, "TESSERA_REACT_SERVICE_VALUE", &manifest);
    expect_code("a use inside a handler",
                "  const f = () => { const nav = useService<Navigator>(); };\n", empty_root,
                "TESSERA_REACT_HOOK_ORDER", &manifest);
}

void test_a_use_without_a_call_still_compiles() {
    const services::ServicesManifest manifest = make_manifest();
    const ServiceFile file("ui/Inbox.tsx",
                           component("  const nav = useService<Navigator>();\n", "<View />"),
                           &manifest);
    check(!file.analyzed.has_errors(),
          "a declared requirement that is never called is not an error: " + file.first_code());
}

} // namespace

int main() {
    test_an_accepted_use_is_recorded();
    test_lowering_produces_the_requirement_and_the_call();
    test_the_evaluator_reaches_the_sink_with_evaluated_arguments();
    test_every_diagnostic_has_its_code();
    test_a_use_without_a_call_still_compiles();
    if (failures == 0) {
        std::cout << "services frontend tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
