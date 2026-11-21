// SPDX-License-Identifier: MIT
//
// `environment` hook lowering (docs/specs/jsx-subset-v1.md section 7.1, issue #692): a TSX
// `useBreakpoint()`/`useTheme()` call becomes an IR `environment[]` entry with the provider, the
// type and whether it is writable, and the component carries it.

#include "lowering_hooks_test_util.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace ir = tessera::compiler::ir;
namespace lowering = tessera::compiler::lowering;

using lowering_hooks_test::Fixture;
using lowering_hooks_test::lower;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string describe(const lowering::HookLoweringResult& result) {
    if (result) {
        return {};
    }
    std::string out;
    for (const auto& diagnostic : result.error()) {
        out += std::string(diagnostic.code) + ": " + diagnostic.message + "; ";
    }
    return out;
}

void test_breakpoint_lowers() {
    const Fixture fixture("function A() {\n"
                          "  const bp = useBreakpoint();\n"
                          "  return <View/>;\n"
                          "}\n");
    const auto result = lower(fixture);
    check(result.has_value(), "useBreakpoint lowers: " + describe(result));
    if (!result) {
        return;
    }
    check(result->environment.size() == 1, "one environment entry");
    if (result->environment.size() != 1) {
        return;
    }
    const ir::Environment& env = result->environment.front();
    check(env.id == "env::bp", "the id is env::<name>");
    check(env.name == "bp", "the name is the local binding");
    check(env.provider == "breakpoint", "the provider comes from the manifest");
    check(env.type == "string", "the type comes from the manifest");
    check(!env.writable, "useBreakpoint is read-only");
}

void test_theme_state_is_writable() {
    const Fixture fixture("function A() {\n"
                          "  const [theme, setTheme] = useThemeState();\n"
                          "  return <View/>;\n"
                          "}\n");
    const auto result = lower(fixture);
    check(result.has_value(), "useThemeState lowers: " + describe(result));
    if (!result) {
        return;
    }
    check(result->environment.size() == 1, "one environment entry");
    if (result->environment.size() != 1) {
        return;
    }
    check(result->environment.front().writable, "useThemeState is writable");
    check(result->environment.front().provider == "theme", "the provider is theme");
}

void test_a_bare_statement_is_a_form_error() {
    // An environment value nobody reads is dead code, and the IR has no id for it.
    const Fixture fixture("function A() {\n"
                          "  useBreakpoint();\n"
                          "  return <View/>;\n"
                          "}\n");
    check(!fixture.analyzed.diagnostics.empty(), "a bare environment call is diagnosed");
    if (!fixture.analyzed.diagnostics.empty()) {
        check(fixture.analyzed.diagnostics.front().code == "TESSERA_REACT_HOOK_FORM",
              "the diagnostic is a form error");
    }
}

} // namespace

int main() {
    test_breakpoint_lowers();
    test_theme_state_is_writable();
    test_a_bare_statement_is_a_form_error();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
