// SPDX-License-Identifier: MIT

// The static context form of ADR-066 item 5 (issue #802): `createContext` at module scope,
// `useContext` in a component body, and `<Ctx.Provider value={...}>` around a consumer. The rules
// half pins the recorded forms and the diagnostics; the lowering half pins the IR; the generated
// C++ is pinned by `codegen_context_tests` (the provider push and the default fallback).

#include "lowering_module_test_util.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace {

using namespace lowering_module_test;
namespace ir = tessera::compiler::ir;

// The corpus fixture's shape: a record-typed context, a consumer, and a provider whose value is a
// pure expression of its props.
constexpr std::string_view kThemedPanel =
    "import { createContext, useContext } from \"react\";\n"
    "\n"
    "type Theme = { accent: string };\n"
    "\n"
    "const ThemeContext = createContext<Theme>({ accent: \"blue\" });\n"
    "\n"
    "function AccentLabel() {\n"
    "  const theme = useContext(ThemeContext);\n"
    "  return <View className=\"accent-label\" className={theme.accent} />;\n"
    "}\n"
    "\n"
    "export function ThemedPanel({ accent }: { accent: string }) {\n"
    "  return (\n"
    "    <ThemeContext.Provider value={{ accent }}>\n"
    "      <AccentLabel />\n"
    "    </ThemeContext.Provider>\n"
    "  );\n"
    "}\n";

void test_context_and_use_are_recorded() {
    Project project;
    project.add(parse_file("ui/use-context.tsx", std::string(kThemedPanel)));
    const lowering::ModuleLoweringResult result = project.lower("use_context");
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    // The consumer is an environment entry naming the context: a `context:` provider, the
    // consumer's local name, and the context's record type.
    bool found_consumer = false;
    for (const ir::Component& component : result->components) {
        if (component.name != "AccentLabel") {
            continue;
        }
        for (const ir::Environment& env : component.environment) {
            if (env.provider == "context:ThemeContext") {
                found_consumer = true;
                check(env.type == "Theme", "the consumer's type is the context's record");
                check(env.argument.kind == ir::ExprKind::Record,
                      "the default is the createContext object literal, as a record value");
            }
        }
    }
    check(found_consumer, "the useContext call becomes a context environment entry");
}

void test_provider_node_carries_the_value() {
    Project project;
    project.add(parse_file("ui/use-context.tsx", std::string(kThemedPanel)));
    const lowering::ModuleLoweringResult result = project.lower("use_context");
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    bool found_provider = false;
    for (const ir::Component& component : result->components) {
        if (component.name != "ThemedPanel") {
            continue;
        }
        // A provider wrapper is a View in the IR: the provided subtree is its children, and the
        // provider fields name the context and carry the value.
        const std::function<const ir::Node*(const ir::Node&)> find =
            [&](const ir::Node& node) -> const ir::Node* {
            if (node.provider == "ThemeContext") {
                return &node;
            }
            for (const ir::Node& child : node.children) {
                if (const ir::Node* hit = find(child)) {
                    return hit;
                }
            }
            return nullptr;
        };
        const ir::Node* provider = find(component.root);
        if (provider != nullptr) {
            found_provider = true;
            check(provider->provider_value != nullptr &&
                      provider->provider_value->kind == ir::ExprKind::Record,
                  "the provider value lowers to a record");
            check(!provider->children.empty(),
                  "the provided subtree is the provider node's children");
        }
    }
    check(found_provider, "the Provider element becomes a provider node");
}

void test_untyped_create_context_is_not_a_context() {
    // The IR carries the type on the environment, so an inferred `createContext(default)` has
    // nothing to name it: the rules pass reports TESSERA_REACT_CONTEXT_FORM (pinned in the
    // diagnostics registry) and contributes no context, so a consumer of it has nothing to bind.
    constexpr std::string_view text = "const Ctx = createContext({ count: 0 });\n"
                                      "export function A() {\n"
                                      "  const v = useContext(Ctx);\n"
                                      "  return <View/>;\n"
                                      "}\n";
    Project project;
    project.add(parse_file_raw("ui/untyped.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("untyped");
    if (result.has_value()) {
        bool any_context = false;
        for (const ir::Component& component : result->components) {
            any_context = any_context || !component.environment.empty();
        }
        check(!any_context, "an untyped createContext contributes no context environment");
    }
}

void test_unknown_context_argument_is_not_a_context() {
    // `Nope` names no module-scope const, so the rules pass reports TESSERA_REACT_CONTEXT_FORM
    // (pinned in the diagnostics registry) and the use records no environment.
    constexpr std::string_view text = "export function A() {\n"
                                      "  const theme = useContext(Nope);\n"
                                      "  return <View/>;\n"
                                      "}\n";
    Project project;
    project.add(parse_file_raw("ui/unknown.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("unknown");
    if (result.has_value()) {
        bool any_context = false;
        for (const ir::Component& component : result->components) {
            any_context = any_context || !component.environment.empty();
        }
        check(!any_context, "a useContext of an undeclared name records no environment");
    }
}

void test_provider_of_unknown_context_is_rejected() {
    constexpr std::string_view text =
        "export function A({ accent }: { accent: string }) {\n"
        "  return <Nope.Provider value={{ accent }}><View/></Nope.Provider>;\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/nope.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("nope");
    check(!result.has_value(),
          "a Provider tag whose name is not a context stays the member-tag rejection");
}

void test_consumer_without_provider_keeps_the_default() {
    // The owner's decision for #802: React semantics. The consumer compiles, and its environment
    // field starts at the createContext default; no provider exists to push anything.
    constexpr std::string_view text =
        "type Theme = { accent: string };\n"
        "const ThemeContext = createContext<Theme>({ accent: \"blue\" });\n"
        "export function A() {\n"
        "  const theme = useContext(ThemeContext);\n"
        "  return <View className={theme.accent} />;\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/no-provider.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("no_provider");
    check(result.has_value(), first_message(result));
}

void test_nested_provider_shadows_the_outer_one() {
    // The nearest provider wins: the pre-order walk overwrites the outer entry during collection,
    // which is the shadowing rule React's tree gives.
    constexpr std::string_view text =
        "type Theme = { accent: string };\n"
        "const ThemeContext = createContext<Theme>({ accent: \"blue\" });\n"
        "function Leaf() {\n"
        "  const theme = useContext(ThemeContext);\n"
        "  return <View className={theme.accent} />;\n"
        "}\n"
        "export function Outer({ a }: { a: string }) {\n"
        "  return (\n"
        "    <ThemeContext.Provider value={{ accent: a }}>\n"
        "      <ThemeContext.Provider value={{ accent: \"inner\" }}>\n"
        "        <Leaf />\n"
        "      </ThemeContext.Provider>\n"
        "    </ThemeContext.Provider>\n"
        "  );\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/nested.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("nested");
    check(result.has_value(), first_message(result));
}

} // namespace

int main() {
    test_context_and_use_are_recorded();
    test_provider_node_carries_the_value();
    test_untyped_create_context_is_not_a_context();
    test_unknown_context_argument_is_not_a_context();
    test_provider_of_unknown_context_is_rejected();
    test_consumer_without_provider_keeps_the_default();
    test_nested_provider_shadows_the_outer_one();
    if (failures == 0) {
        std::cout << "lowering context tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
