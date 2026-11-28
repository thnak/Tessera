// SPDX-License-Identifier: MIT
//
// The shim packages' front-end half (docs/specs/router-shim-v1.md,
// docs/specs/store-i18n-shims-v1.md; issues #549 and #550): what `useNavigate` lowers to, and the
// diagnostic of every form outside a coverage table. One test per covered form and one per rejected
// form, as the specs promise.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/jsx/shim_expansion.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace eval = tessera::compiler::eval;

// Parse, run the shim pass (through the hook-expansion entry the driver calls), analyse.
struct ShimFile {
    explicit ShimFile(std::string text)
        : source(std::move(text)),
          parsed(jsx::parse_module(source)),
          map(source),
          sink(map, jsx::Limits{}) {
        if (parsed.root != nullptr) {
            metrics = jsx::expand_custom_hooks(*parsed.root, map, jsx::FrontEndOptions{}, sink);
            analyzed = rules::analyze_module(*parsed.root, map, jsx::Limits{}, nullptr,
                                             &metrics.call_sites);
        }
    }
    ShimFile(const ShimFile&) = delete;
    ShimFile& operator=(const ShimFile&) = delete;

    // The first error of the shim pass, else of the rules, else empty.
    [[nodiscard]] std::string first_code() const {
        for (const jsx::Diagnostic& diagnostic : sink.diagnostics()) {
            if (diagnostic.severity == jsx::Severity::Error) {
                return std::string(diagnostic.code);
            }
        }
        for (const jsx::Diagnostic& diagnostic : analyzed.diagnostics) {
            if (diagnostic.severity == jsx::Severity::Error) {
                return std::string(diagnostic.code);
            }
        }
        return {};
    }
    [[nodiscard]] std::string first_suggestion() const {
        for (const jsx::Diagnostic& diagnostic : sink.diagnostics()) {
            return diagnostic.suggestion;
        }
        return {};
    }
    [[nodiscard]] lowering::ModuleSource module_source() const {
        return lowering::ModuleSource{"ui/Nav.tsx", parsed.root.get(), &analyzed, &map};
    }

    std::string source;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    jsx::DiagnosticSink sink;
    jsx::ExpansionMetrics metrics;
    rules::AnalyzedModule analyzed;
};

std::string app(std::string_view imports, std::string_view body, std::string_view root) {
    return std::string(imports) + "\nexport function Nav({ id }: { id: string }) {\n" +
           std::string(body) + "  return " + std::string(root) + ";\n}\n";
}

constexpr std::string_view kImport = "import { useNavigate } from \"react-router-dom\";\nimport { "
                                     "useState, useEffect } from \"tessera\";";
constexpr std::string_view kBody = "  const navigate = useNavigate();\n";

// The IR component of a clean file, or null after reporting.
struct Lowered {
    explicit Lowered(std::string text)
        : file(std::move(text)) {
        check(file.parsed.ok(), "parses: " + codes(file.parsed));
        check(file.first_code().empty(), "no error: " + file.first_code());
        const std::vector<lowering::ModuleSource> sources{file.module_source()};
        result = lowering::lower_project(sources, "nav");
        check(result.has_value(), first_message(result));
        if (result) {
            nav = find_component(*result, "Nav");
        }
    }
    ShimFile file;
    lowering::ModuleLoweringResult result = std::unexpected(lowering::NodeLoweringErrors{});
    const ir::Component* nav = nullptr;
};

const ir::Action* first_call(const Lowered& lowered, std::size_t handler) {
    if (lowered.nav == nullptr || lowered.nav->handlers.size() <= handler ||
        lowered.nav->handlers[handler].actions.empty()) {
        return nullptr;
    }
    return &lowered.nav->handlers[handler].actions[0];
}

void test_navigate_lowers_to_the_router_service() {
    Lowered lowered(app(kImport, kBody, "<View onClick={() => navigate(\"/orders\")} />"));
    check(lowered.nav != nullptr, "the component lowers");
    if (lowered.nav == nullptr) {
        return;
    }
    check(lowered.nav->services.size() == 1 &&
              lowered.nav->services[0].registry_id == "tessera.router" &&
              lowered.nav->services[0].type == "tessera::appfw::RouterService" &&
              lowered.nav->services[0].header == "tessera/appfw/router_service.hpp",
          "the requirement is the built-in router service, with no ui/services.json");
    const ir::Action* call = first_call(lowered, 0);
    check(call != nullptr && call->kind == ir::ActionKind::CallService && call->method == "push" &&
              call->args.size() == 1,
          "navigate(path) is a push call");
}

void test_replace_back_and_an_expression_path() {
    Lowered replace(
        app(kImport, kBody, "<View onClick={() => navigate(\"/a/\" + id, { replace: true })} />"));
    const ir::Action* call = first_call(replace, 0);
    check(call != nullptr && call->method == "replace" && call->args.size() == 1,
          "navigate(path, { replace: true }) is a replace call with the expression path");

    Lowered push_false(
        app(kImport, kBody, "<View onClick={() => navigate(\"/a\", { replace: false })} />"));
    call = first_call(push_false, 0);
    check(call != nullptr && call->method == "push", "{ replace: false } is a push");

    Lowered templated(app(kImport, kBody, "<View onClick={() => navigate(`/orders/${id}`)} />"));
    call = first_call(templated, 0);
    check(call != nullptr && call->method == "push", "a template-literal path is a push");

    Lowered back(app(kImport, kBody, "<View onClick={() => navigate(-1)} />"));
    call = first_call(back, 0);
    check(call != nullptr && call->method == "back" && call->args.empty(),
          "navigate(-1) is back with no argument");
}

void test_an_aliased_hook_and_the_react_router_specifier() {
    Lowered lowered(app("import { useNavigate as useNav } from \"react-router\";",
                        "  const go = useNav();\n", "<View onClick={() => go(\"/x\")} />"));
    const ir::Action* call = first_call(lowered, 0);
    check(call != nullptr && call->method == "push",
          "an alias and the react-router specifier work");
}

void test_navigate_in_an_effect() {
    Lowered lowered(
        app(kImport,
            std::string(kBody) +
                "  const [n, setN] = useState<int>(0);\n"
                "  // Note: bind::go\n  useEffect(() => { navigate(\"/home\"); }, [navigate]);\n",
            "<View />"));
    check(lowered.nav != nullptr,
          "an effect body with navigate lowers, and navigate is dropped from its dependency list");
}

struct Recorded {
    std::string method;
    std::string arg;
};
void on_call(void* context, const std::string&, const std::string& method,
             const std::vector<eval::Value>& args) {
    auto& recorded = *static_cast<Recorded*>(context);
    recorded.method = method;
    recorded.arg = args.empty() || args[0].as_string() == nullptr ? "" : *args[0].as_string();
}

void test_the_evaluator_reaches_the_sink() {
    Lowered lowered(app(kImport, kBody, "<View onClick={() => navigate(id)} />"));
    if (lowered.nav == nullptr || lowered.nav->handlers.empty()) {
        check(false, "handler lowers");
        return;
    }
    eval::Evaluator evaluator;
    Recorded recorded;
    evaluator.set_service_call_sink(&on_call, &recorded);
    eval::Scope scope{{"prop::id", eval::Value::of_string("/orders/42")}};
    const auto ran = evaluator.run(lowered.nav->handlers[0].actions, scope);
    check(ran.has_value(), "the handler runs");
    check(recorded.method == "push" && recorded.arg == "/orders/42",
          "the preview sees push with the evaluated path");
}

void expect_code(std::string_view what, const std::string& text, std::string_view code) {
    const ShimFile file(text);
    check(file.first_code() == code,
          std::string(what) + ": expected " + std::string(code) + ", got " + file.first_code());
}

void test_rejected_router_forms() {
    const auto click = [](std::string_view call) {
        return app(kImport, kBody, "<View onClick={() => " + std::string(call) + "} />");
    };
    expect_code("a number other than -1", click("navigate(2)"), "TESSERA_SHIM_FORM");
    expect_code("a location object", click("navigate({ pathname: \"/a\" })"), "TESSERA_SHIM_FORM");
    expect_code("an unsupported option", click("navigate(\"/a\", { state: id })"),
                "TESSERA_SHIM_FORM");
    expect_code("a non-literal replace", click("navigate(\"/a\", { replace: true && true })"),
                "TESSERA_SHIM_FORM");
    expect_code("no arguments", click("navigate()"), "TESSERA_SHIM_FORM");
    expect_code("navigate(-1) with options", click("navigate(-1, { replace: true })"),
                "TESSERA_SHIM_FORM");
    expect_code("an argument to useNavigate",
                app(kImport, "  const navigate = useNavigate(\"x\");\n", "<View />"),
                "TESSERA_SHIM_FORM");
    expect_code("navigate called while rendering",
                app(kImport, std::string(kBody) + "  const ok = navigate(\"/a\");\n", "<View />"),
                "TESSERA_REACT_SERVICE_CALL_IN_RENDER");
    expect_code("useNavigate in a custom hook",
                std::string(kImport) +
                    "\nfunction useGo() {\n  const n = useNavigate();\n  return { n };\n}\n"
                    "export function Nav() { return <View />; }\n",
                "TESSERA_SHIM_FORM");
}

void test_the_coverage_table_diagnostics() {
    const auto with = [](std::string_view import_line) { return app(import_line, "", "<View />"); };
    expect_code("a router wrapper", with("import { BrowserRouter } from \"react-router-dom\";"),
                "TESSERA_SHIM_UNSUPPORTED");
    expect_code("a name the table does not list",
                with("import { useRoutes } from \"react-router-dom\";"),
                "TESSERA_SHIM_UNSUPPORTED");
    expect_code("a covered name whose lowering is pending",
                with("import { useParams } from \"react-router-dom\";"), "TESSERA_SHIM_PENDING");
    expect_code("Link", with("import { Link } from \"react-router-dom\";"), "TESSERA_SHIM_PENDING");
    expect_code("zustand create", with("import { create } from \"zustand\";"),
                "TESSERA_SHIM_PENDING");
    expect_code("zustand middleware", with("import { persist } from \"zustand\";"),
                "TESSERA_SHIM_UNSUPPORTED");
    expect_code("useTranslation", with("import { useTranslation } from \"react-i18next\";"),
                "TESSERA_SHIM_PENDING");
    expect_code("Trans", with("import { Trans } from \"react-i18next\";"),
                "TESSERA_SHIM_UNSUPPORTED");
    expect_code("i18next init", with("import { init } from \"i18next\";"),
                "TESSERA_SHIM_UNSUPPORTED");
    expect_code("i18next t", with("import { t } from \"i18next\";"), "TESSERA_SHIM_PENDING");

    // The message names the supported form, never just "not supported".
    const ShimFile wrapper(with("import { BrowserRouter } from \"react-router-dom\";"));
    check(wrapper.first_suggestion().find("NavController") != std::string::npos,
          "the hint names the replacement: " + wrapper.first_suggestion());
    const ShimFile link(with("import { Link } from \"react-router-dom\";"));
    check(link.first_suggestion().find("navigate(") != std::string::npos,
          "a pending name's hint names the form to use meanwhile");
}

void test_a_file_without_a_shim_is_untouched() {
    const ShimFile file(app("import { useState } from \"tessera\";", "", "<View />"));
    check(file.sink.diagnostics().empty() && file.first_code().empty(),
          "no shim import, no diagnostic");
}

} // namespace

int main() {
    test_navigate_lowers_to_the_router_service();
    test_replace_back_and_an_expression_path();
    test_an_aliased_hook_and_the_react_router_specifier();
    test_navigate_in_an_effect();
    test_the_evaluator_reaches_the_sink();
    test_rejected_router_forms();
    test_the_coverage_table_diagnostics();
    test_a_file_without_a_shim_is_untouched();
    return finish("shim_router");
}
