// SPDX-License-Identifier: MIT
//
// `useObservable(svc.member)` from TSX (ADR-050 open question 6,
// docs/design/observable-render-read.md, docs/specs/services-manifest-v1.md): the analysis half
// (what is accepted and every diagnostic), the lowering half (the IR `observables[]` entry and the
// `obs::` reads), the IR validation of the entry, the evaluator reading it from its scope, and the
// shape of the generated base. The generated C++ is compiled and run by
// `codegen_observable_tsx_runtime`.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;
namespace eval = tessera::compiler::eval;
namespace ids = tessera::compiler::ids;
namespace services = tessera::compiler::services;

services::ServicesManifest make_manifest() {
    services::ManifestService nav;
    nav.id = "demo.nav";
    nav.name = "Navigator";
    nav.cpp_type = "demo::Navigator";
    nav.header = "demo/navigator.hpp";
    nav.methods.push_back({"push", {{"route", "string"}}, "void"});
    nav.observables.push_back({"title", "string"});
    nav.observables.push_back({"unread", "int"});
    nav.observables.push_back({"route", "Route"});
    services::ServicesManifest manifest;
    manifest.services.push_back(std::move(nav));
    return manifest;
}

struct ObservableFile {
    ObservableFile(std::string text, const services::ServicesManifest* manifest)
        : source(std::move(text)),
          parsed(jsx::parse_module(source)),
          map(source),
          analyzed(rules::analyze_module(*parsed.root, map, {}, nullptr, nullptr, manifest)) {}
    ObservableFile(const ObservableFile&) = delete;
    ObservableFile& operator=(const ObservableFile&) = delete;

    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{"ui/Header.tsx", parsed.root.get(), &analyzed, &map};
    }
    [[nodiscard]] std::string first_code() const {
        return analyzed.diagnostics.empty() ? std::string()
                                            : std::string(analyzed.diagnostics.front().code);
    }

    std::string source;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    rules::AnalyzedModule analyzed;
};

std::string component(std::string_view body, std::string_view root) {
    return "import { useState, useService, useObservable } from \"tessera\";\n"
           "type Route = { title: string; depth: int };\n"
           "export function Header() {\n" +
           std::string(body) + "  return " + std::string(root) + ";\n}\n";
}

constexpr std::string_view kBody = "  const nav = useService<Navigator>();\n"
                                   "  const title = useObservable(nav.title);\n"
                                   "  const unread = useObservable(nav.unread);\n";
constexpr std::string_view kRoot =
    "<View><Text>{title}</Text><Text>{`${unread} new`}</Text></View>";

lowering::ModuleLoweringResult lower(const ObservableFile& file) {
    const std::vector<lowering::ModuleSource> sources{file.module_source()};
    return lowering::lower_project(sources, "demo");
}

// The IR module a driver assembles from the lowered one (the lowered module is move-only).
ir::IrModule to_module(lowering::ModuleLoweringResult& lowered) {
    ir::IrModule module;
    module.name = lowered->name;
    module.components = std::move(lowered->components);
    module.bindings = std::move(lowered->bindings);
    module.stylesheets = std::move(lowered->stylesheets);
    module.types = std::move(lowered->types);
    // Validation requires the numeric ids the build's identity pass assigns.
    check(ids::assign_identity(module, "demo").has_value(), "identity is assigned");
    return module;
}

void test_an_accepted_use_is_recorded() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component(kBody, kRoot), &manifest);
    check(file.parsed.ok(), "the fixture parses: " + codes(file.parsed));
    check(!file.analyzed.has_errors(), "an accepted use analyses cleanly: " + file.first_code());
    const auto& observables = file.analyzed.components.front().observables;
    check(observables.size() == 2, "both uses are recorded");
    check(observables.size() == 2 && observables[0].name == "title" &&
              observables[0].service_binding == "nav" && observables[0].member == "title" &&
              observables[0].type == "string" && observables[1].type == "int",
          "each use names the binding, the member and the manifest's type");
}

void test_lowering_produces_the_entries_and_the_reads() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component(kBody, kRoot), &manifest);
    lowering::ModuleLoweringResult result = lower(file);
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* header = find_component(*result, "Header");
    check(header != nullptr && header->observables.size() == 2, "two observables lower");
    if (header == nullptr || header->observables.size() != 2) {
        return;
    }
    const ir::Observable& title = header->observables[0];
    check(title.id == "obs::title" && title.service == "svc::nav" && title.member == "title" &&
              title.type == "string",
          "the entry carries the id, the service, the member and the type");
    check(header->services.size() == 1, "the service requirement is still lowered");
    const std::string json = describe(*result);
    check(json.find("\"observables\"") != std::string::npos &&
              json.find("obs::unread") != std::string::npos,
          "the IR JSON lists the observables and the reads name them");
    // The shared fixture declares a `Route` record this component does not read; that warning is
    // not what is under test.
    std::string problem;
    for (const auto& diagnostic : ir::validate(to_module(result))) {
        if (diagnostic.code != "TESSERA_IR_UNUSED_TYPE" && problem.empty()) {
            problem = diagnostic.code + " " + diagnostic.message;
        }
    }
    check(problem.empty(), "the lowered module validates: " + problem);
}

void test_a_record_is_read_by_field() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component("  const nav = useService<Navigator>();\n"
                                        "  const route = useObservable(nav.route);\n",
                                        "<Text>{route.title}</Text>"),
                              &manifest);
    check(!file.analyzed.has_errors(), "a record observable analyses: " + file.first_code());
    const lowering::ModuleLoweringResult result = lower(file);
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    check(describe(*result).find("obs::route.title") != std::string::npos,
          "a record field read keeps the dotted id");
}

void test_a_missing_record_member_is_rejected_by_the_validator() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component("  const nav = useService<Navigator>();\n"
                                        "  const route = useObservable(nav.route);\n",
                                        "<Text>{route.nope}</Text>"),
                              &manifest);
    lowering::ModuleLoweringResult result = lower(file);
    if (!result) {
        return; // rejected during lowering: also acceptable, the read never reaches codegen
    }
    check(!ir::validate(to_module(result)).empty(),
          "a field the record does not have is a diagnostic");
}

void test_the_evaluator_reads_the_scope() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component(kBody, kRoot), &manifest);
    const lowering::ModuleLoweringResult result = lower(file);
    if (!result) {
        check(false, "the fixture lowers");
        return;
    }
    ir::Expr read;
    read.kind = ir::ExprKind::Ref;
    read.id = "obs::title";
    eval::Evaluator evaluator;
    const eval::Scope scope{{"obs::title", eval::Value::of_string("Inbox")}};
    const auto value = evaluator.evaluate(read, scope);
    check(value.has_value() && value->as_string() != nullptr && *value->as_string() == "Inbox",
          "an `obs::` ref reads the host's current value from the scope");
    const auto missing = evaluator.evaluate(read, eval::Scope{});
    check(!missing.has_value() && missing.error().code == "TESSERA_IR_EVAL_UNKNOWN_REF",
          "an unset observable is the same unknown-ref error as any unbound name");
}

void test_the_generated_base_subscribes_at_mount() {
    const services::ServicesManifest manifest = make_manifest();
    const ObservableFile file(component(kBody, kRoot), &manifest);
    lowering::ModuleLoweringResult lowered = lower(file);
    if (!lowered) {
        check(false, "the fixture lowers");
        return;
    }
    const ir::IrModule module = to_module(lowered);
    const codegen::CodegenResult generated = codegen::generate(module, {}, std::nullopt);
    check(generated.has_value(), "the module generates");
    if (!generated || generated->empty()) {
        return;
    }
    const std::string& text = generated->front().text;
    const auto has = [&](std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    check(has("#include \"tessera/appfw/observable.hpp\""), "the runtime header is included");
    check(has("obs_title_ = &std::get<demo::Navigator*>(services_)->title();"),
          "attach_services caches the observable's address");
    check(has("obs_title_value()"), "reads go through the accessor");
    check(has("obs_title_sub_ = obs_title_->subscribe("), "mount subscribes");
    check(has("obs_title_sub_.unsubscribe();"), "unmount unsubscribes");
    check(has("kBit_obs_title_"), "the observable has its own dirty bit");
    // A component without observables generates none of it.
    const ObservableFile plain(component("", "<View />"), &manifest);
    lowering::ModuleLoweringResult plain_lowered = lower(plain);
    if (plain_lowered) {
        const ir::IrModule plain_module = to_module(plain_lowered);
        const codegen::CodegenResult out = codegen::generate(plain_module, {}, std::nullopt);
        check(out.has_value() && !out->empty() &&
                  out->front().text.find("obs_") == std::string::npos &&
                  out->front().text.find("observable.hpp") == std::string::npos,
              "a component without useObservable generates no observable code");
    }
}

void expect_code(std::string_view name, std::string_view body, std::string_view root,
                 std::string_view code, const services::ServicesManifest* manifest) {
    const ObservableFile file(component(body, root), manifest);
    check(file.first_code() == code,
          std::string(name) + ": expected " + std::string(code) + ", got " + file.first_code());
}

void test_every_diagnostic_has_its_code() {
    const services::ServicesManifest manifest = make_manifest();
    constexpr std::string_view nav = "  const nav = useService<Navigator>();\n";
    const std::string empty_root = "<View />";
    expect_code("a member the manifest does not list",
                std::string(nav) + "  const x = useObservable(nav.missing);\n", empty_root,
                "TESSERA_REACT_HOOK_ARG", &manifest);
    expect_code("a method is not an observable",
                std::string(nav) + "  const x = useObservable(nav.push);\n", empty_root,
                "TESSERA_REACT_HOOK_ARG", &manifest);
    expect_code("a variable argument",
                std::string(nav) + "  const t = 1;\n  const x = useObservable(t);\n", empty_root,
                "TESSERA_REACT_HOOK_ARG", &manifest);
    expect_code("a conditional argument",
                std::string(nav) + "  const x = useObservable(true ? nav.title : nav.title);\n",
                empty_root, "TESSERA_REACT_HOOK_ARG", &manifest);
    expect_code("not a service binding",
                "  const nav = 1;\n  const x = useObservable(nav.title);\n", empty_root,
                "TESSERA_REACT_HOOK_ARG", &manifest);
    expect_code("a destructured binding",
                std::string(nav) + "  const [x] = useObservable(nav.title);\n", empty_root,
                "TESSERA_REACT_HOOK_FORM", &manifest);
    expect_code("no argument", std::string(nav) + "  const x = useObservable();\n", empty_root,
                "TESSERA_REACT_HOOK_FORM", &manifest);
    expect_code("a use inside a handler",
                std::string(nav) + "  const f = () => { const x = useObservable(nav.title); };\n",
                empty_root, "TESSERA_REACT_HOOK_ORDER", &manifest);
    expect_code("read before its service",
                "  const x = useObservable(nav.title);\n" + std::string(nav), empty_root,
                "TESSERA_REACT_HOOK_ORDER", &manifest);
}

} // namespace

int main() {
    test_an_accepted_use_is_recorded();
    test_lowering_produces_the_entries_and_the_reads();
    test_a_record_is_read_by_field();
    test_a_missing_record_member_is_rejected_by_the_validator();
    test_the_evaluator_reads_the_scope();
    test_the_generated_base_subscribes_at_mount();
    test_every_diagnostic_has_its_code();
    if (failures == 0) {
        std::cout << "observable frontend tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
