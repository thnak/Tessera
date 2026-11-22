// SPDX-License-Identifier: MIT

// Control flow in a handler body (ADR-066 item 2): `if`/`else`, `const` and an early `return;`
// lower to the `If` action, an inlined local, and an early end of the list. The differential
// property the ADR asks for is that the evaluator and the generated C++ agree; the evaluator half
// is here, and `codegen_handler_control_tests` covers the generated C++.

#include "lowering_hooks_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_hooks_test;

// A component with one handler whose body is `body`, so a test states only the statements it is
// about.
std::string component_with_handler(std::string_view body) {
    std::string source = "function A({ disabled }: { disabled: boolean }) {\n"
                         "  const [busy, setBusy] = useState<boolean>(false);\n"
                         "  const submit = () => {\n";
    source += body;
    source += "  };\n"
              "  return <View onClick={submit} />;\n"
              "}";
    return source;
}

// The `describe` text of a component with one `useState` and one inline handler, so a test states
// only the actions it is about. `actions` is the body of the `handler::1` block.
std::string expected(std::string_view actions) {
    std::string out = "component comp::A\n"
                      "state state::busy busy bool {\"kind\":\"literal\",\"value\":false}\n"
                      "handler handler::1 params=[] source=ui/Counter.tsx:3:18\n";
    out += actions;
    out += "index setter setBusy=state::busy\n"
           "index handler submit=handler::1 (inline)\n"
           "next handler 2\n";
    return out;
}

void test_if_with_early_return() {
    // The corpus fixture's shape: an `if` whose branch is a bare `return;`. The branch is empty
    // because the return ends it, and the statement after the `if` still runs.
    const auto fixture = parse(component_with_handler("    if (disabled) {\n"
                                                      "      return;\n"
                                                      "    }\n"
                                                      "    setBusy(!busy);\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  if {\"kind\":\"ref\",\"id\":\"prop::disabled\"} then []\n"
                      "  set state::busy = "
                      "{\"kind\":\"unary\",\"op\":\"!\",\"value\":{\"kind\":\"ref\","
                      "\"id\":\"state::busy\"}}\n"),
             "an `if` with an early return, then the statement after it");
}

void test_if_else() {
    const auto fixture = parse(component_with_handler("    if (disabled) {\n"
                                                      "      setBusy(false);\n"
                                                      "    } else {\n"
                                                      "      setBusy(true);\n"
                                                      "    }\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  if {\"kind\":\"ref\",\"id\":\"prop::disabled\"} then [set state::busy = "
                      "{\"kind\":\"literal\",\"value\":false}; ] else [set state::busy = "
                      "{\"kind\":\"literal\",\"value\":true}; ]\n"),
             "both branches of an `if`/`else`");
}

void test_else_if_chain() {
    // `else if` is the same node nested in the else branch, so a chain needs no new kind.
    const auto fixture = parse(component_with_handler("    if (disabled) {\n"
                                                      "      setBusy(false);\n"
                                                      "    } else if (busy) {\n"
                                                      "      setBusy(true);\n"
                                                      "    }\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  if {\"kind\":\"ref\",\"id\":\"prop::disabled\"} then [set state::busy = "
                      "{\"kind\":\"literal\",\"value\":false}; ] else [if "
                      "{\"kind\":\"ref\",\"id\":\"state::busy\"} then [set state::busy = "
                      "{\"kind\":\"literal\",\"value\":true}; ]; ]\n"),
             "`else if` nests in the else branch");
}

void test_const_local_is_inlined() {
    // `const next = !busy;` is a local, so it produces no action and is inlined at its use.
    const auto fixture = parse(component_with_handler("    const next = !busy;\n"
                                                      "    setBusy(next);\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  set state::busy = "
                      "{\"kind\":\"unary\",\"op\":\"!\",\"value\":{\"kind\":\"ref\","
                      "\"id\":\"state::busy\"}}\n"),
             "a handler local is inlined, not referenced");
}

void test_const_inside_a_branch() {
    const auto fixture = parse(component_with_handler("    if (disabled) {\n"
                                                      "      const next = !busy;\n"
                                                      "      setBusy(next);\n"
                                                      "    }\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  if {\"kind\":\"ref\",\"id\":\"prop::disabled\"} then [set state::busy = "
                      "{\"kind\":\"unary\",\"op\":\"!\",\"value\":{\"kind\":\"ref\","
                      "\"id\":\"state::busy\"}}; ]\n"),
             "a local declared inside a branch is inlined there");
}

void test_nested_if() {
    const auto fixture = parse(component_with_handler("    if (disabled) {\n"
                                                      "      if (busy) {\n"
                                                      "        setBusy(false);\n"
                                                      "      }\n"
                                                      "    }\n"));
    const auto lowered = lower(*fixture);
    check(lowered.has_value(), "the handler lowers: " + describe_errors(*fixture, lowered));
    if (!lowered) {
        return;
    }
    check_eq(describe(*lowered),
             expected("  if {\"kind\":\"ref\",\"id\":\"prop::disabled\"} then [if "
                      "{\"kind\":\"ref\",\"id\":\"state::busy\"} then [set state::busy = "
                      "{\"kind\":\"literal\",\"value\":false}; ]; ]\n"),
             "an `if` inside an `if`");
}

void test_return_with_a_value_is_rejected() {
    // A value return is only a preview-body return, which `lower_preview` handles; in an inline
    // handler it has nowhere to go. The analyzer catches it first, because such a body is not made
    // of handler statements, so the diagnostic is `TESSERA_REACT_BIND_MISSING` rather than a
    // lowering error. Either way the component does not compile. `parse` asserts a clean analysis,
    // so this case builds its fixture directly.
    const Fixture fixture(component_with_handler("    if (disabled) {\n"
                                                 "      return busy;\n"
                                                 "    }\n"));
    check(fixture.parsed.ok(), "the fixture parses: " + codes(fixture.parsed));
    check(!fixture.analyzed.components.empty() && !fixture.analyzed.components[0].ok,
          "a value return in an inline handler is rejected");
    const auto lowered = lower(fixture);
    check(!lowered.has_value(), "the component produces no IR");
}

void test_loop_stays_rejected() {
    // ADR-066 item 2 keeps `let`, loops and mutation in native handlers; only `if`/`else`, `const`
    // and an early `return` are accepted. A loop is a parse error, because the statement grammar
    // has no loop form at all.
    const auto loop = jsx::parse_module(component_with_handler("    for (;;) {\n"
                                                               "      setBusy(true);\n"
                                                               "    }\n"));
    check(!loop.ok(), "a loop in a handler body is rejected");
    check_eq(codes(loop), "TESSERA_JSX_SUBSET_STATEMENT;", "the code");
}

} // namespace

int main() {
    test_if_with_early_return();
    test_if_else();
    test_else_if_chain();
    test_const_local_is_inlined();
    test_const_inside_a_branch();
    test_nested_if();
    test_return_with_a_value_is_rejected();
    test_loop_stays_rejected();
    return finish("lowering handler control tests");
}
