// SPDX-License-Identifier: MIT

// Decision 8 of docs/design/jsx-ir-lowering.md: a `const` local is inlined at each use, so a
// component that reads one compiles instead of failing with TESSERA_IR_UNRESOLVED. The tests cover
// the four required cases: a plain local, a local used twice, a self-referential local, and a local
// whose initializer is itself rejected.

#include "lowering_nodes_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_nodes_test;

// `const state = io.state;` then `{state}`: the IR carries the initializer's expression, not a
// `local::state` reference. `io` is the props parameter, so `io.state` is the prop `state`
// (decision 8: `props.x` of a non-destructured parameter is a prop reference).
void test_local_inlines_its_initializer() {
    const auto fixture =
        parse(component("  const state = io.state;\n", "<View>{state}</View>", "io: Io"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a component that reads a local lowers");
    if (!result) {
        return;
    }
    const std::string text = describe(result->component.root);
    check(text.find("prop::state") != std::string::npos,
          "the local inlines the initializer's reference: " + text);
    check(text.find("local::") == std::string::npos, "no `local::` id is emitted: " + text);
}

// A local read twice inlines twice: the IR has two copies of the initializer, not one shared ref.
void test_local_used_twice_inlines_twice() {
    const auto fixture = parse(component("  const [count, setCount] = useState<int>(0);\n"
                                         "  const label = count;\n",
                                         "<View><Text>{label}</Text><Text>{label}</Text></View>"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a component that reads a local twice lowers");
    if (!result) {
        return;
    }
    const std::string text = describe(result->component.root);
    const std::size_t first = text.find("state::count");
    check(first != std::string::npos, "the first use inlines the initializer: " + text);
    check(first != std::string::npos && text.find("state::count", first + 1) != std::string::npos,
          "the second use inlines the initializer too: " + text);
}

// `const a = a;` is a cycle, not a stack overflow: it is reported with a named code.
void test_self_referential_local_is_rejected() {
    const auto fixture = parse(component("  const a = a;\n", "<View>{a}</View>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a self-referential local produces no component");
    check_eq(first_code(result), "TESSERA_IR_LOCAL_CYCLE", "the cycle code");
}

// A two-step cycle is caught as well, so the recursion is bounded by the number of locals.
void test_indirect_local_cycle_is_rejected() {
    const auto fixture = parse(component("  const a = b;\n  const b = a;\n", "<View>{a}</View>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "an indirect local cycle produces no component");
    check_eq(first_code(result), "TESSERA_IR_LOCAL_CYCLE", "the indirect cycle code");
}

// A local whose initializer the subset rejects reports that rejection, not TESSERA_IR_UNRESOLVED:
// the diagnostic names the real problem.
void test_rejected_initializer_reports_its_own_code() {
    const auto fixture = parse(component("  const rows = { a: 1 };\n", "<View>{rows}</View>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "a local with a rejected initializer produces no component");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the initializer's own code");
}

// An object pattern binds one field of the initializer: `const { state } = io;` reads `io.state`.
void test_object_pattern_local_inlines_its_field() {
    const auto fixture =
        parse(component("  const { state } = io;\n", "<View>{state}</View>", "{ io }: { io: Io }"));
    const auto result = lower(*fixture);
    check(result.has_value(), "an object-pattern local lowers");
    if (!result) {
        return;
    }
    const std::string text = describe(result->component.root);
    check(text.find("prop::io.state") != std::string::npos,
          "the pattern inlines the initializer's field: " + text);
}

// A local read inside a handler body inlines there too, so a handler argument is not left
// unresolved.
void test_local_in_a_handler_body() {
    const auto fixture = parse(component("  const [count, setCount] = useState<int>(0);\n"
                                         "  const step = count;\n",
                                         "<View onClick={() => setCount(step)} />"));
    const auto result = lower(*fixture);
    check(result.has_value(), "a local read in a handler body lowers");
    if (!result) {
        return;
    }
    bool found = false;
    for (const ir::Handler& handler : result->component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.value && action.value->kind == ir::ExprKind::Ref &&
                action.value->id == "state::count") {
                found = true;
            }
        }
    }
    check(found, "the handler action inlines the local's initializer");
}

// A name that is neither a declaration nor a local keeps the old diagnostic.
void test_unknown_name_stays_unresolved() {
    const auto fixture = parse(component("", "<View>{nope}</View>"));
    const auto result = lower(*fixture);
    check(!result.has_value(), "an unknown name produces no component");
    check_eq(first_code(result), "TESSERA_IR_UNRESOLVED", "the unresolved code");
}

} // namespace

int main() {
    test_local_inlines_its_initializer();
    test_local_used_twice_inlines_twice();
    test_self_referential_local_is_rejected();
    test_indirect_local_cycle_is_rejected();
    test_rejected_initializer_reports_its_own_code();
    test_object_pattern_local_inlines_its_field();
    test_local_in_a_handler_body();
    test_unknown_name_stays_unresolved();
    return finish("lowering local inline tests");
}
