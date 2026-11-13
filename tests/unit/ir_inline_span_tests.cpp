// SPDX-License-Identifier: MIT
//
// IR validation of inline spans (issue #229, ir-v1.md section 3.6): a Text with spans has no text
// of its own, every child is a literal-text span, nested spans and span attributes are rejected,
// the metric-style rule holds, and the count/byte caps are TESSERA_IR_SPAN.

#include "ir_samples.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
using ir_test::span;

int failures = 0;

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
    return condition;
}

// One Text root with `span_count` literal spans.
ir::IrModule make_module(std::size_t span_count = 2) {
    ir::IrModule module;
    module.name = "span_test";
    ir::Component component;
    component.id = "comp::A";
    component.component_id = "1";
    component.name = "A";
    component.source = span(0, 20, 1, 1, 12, 2);
    ir::Node& root = component.root;
    root.kind = ir::NodeKind::Text;
    root.id = "node::1";
    root.ui_id = "2";
    root.source = span(10, 5, 1, 10, 1, 15);
    for (std::size_t index = 0; index < span_count; ++index) {
        ir::Node child;
        child.kind = ir::NodeKind::Text;
        child.id = "node::" + std::to_string(index + 2);
        child.ui_id = std::to_string(10 + index);
        child.source = span(12 + static_cast<std::uint32_t>(index), 1, 1, 12, 1, 13);
        child.text = ir::make_literal(std::string("x"));
        root.children.push_back(std::move(child));
    }
    module.components.push_back(std::move(component));
    return module;
}

std::string codes_of(const ir::IrModule& module) {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        codes += diagnostic.code + ";";
    }
    return codes;
}

bool has_code(const ir::IrModule& module, std::string_view code) {
    return codes_of(module).find(code) != std::string::npos;
}

void test_valid_spans() {
    const ir::IrModule module = make_module();
    check(codes_of(module).empty(), "a Text with literal spans is valid: " + codes_of(module));
}

void test_parent_text_rules() {
    ir::IrModule module = make_module();
    module.components[0].root.text = ir::make_literal(std::string("own text"));
    check(has_code(module, "TESSERA_IR_SPAN"), "a spanned Text may not carry its own text");

    ir::IrModule nested = make_module();
    ir::Node inner;
    inner.kind = ir::NodeKind::Text;
    inner.id = "node::99";
    inner.ui_id = "99";
    inner.source = span(0, 1, 1, 1, 1, 2);
    inner.text = ir::make_literal(std::string("y"));
    nested.components[0].root.children[0].children.push_back(std::move(inner));
    check(has_code(nested, "TESSERA_IR_SPAN"), "a nested span is rejected");
}

void test_span_text_rules() {
    ir::IrModule module = make_module();
    module.components[0].root.children[0].text = ir::make_literal(std::int64_t{3});
    check(has_code(module, "TESSERA_IR_SPAN"), "a non-string span text is rejected");

    ir::IrModule dynamic = make_module();
    dynamic.components[0].root.children[0].text = ir::make_ref("state::value");
    check(has_code(dynamic, "TESSERA_IR_SPAN") || has_code(dynamic, "TESSERA_IR_UNRESOLVED"),
          "a non-literal span text is rejected");
}

void test_span_attributes() {
    ir::IrModule metric = make_module();
    metric.components[0].root.children[0].style.emplace("font-size", ir::make_literal("12px"));
    check(has_code(metric, "TESSERA_IR_SPAN"), "a span may not override a metric property");

    ir::IrModule tab = make_module();
    tab.components[0].root.children[0].props.emplace("tabIndex", ir::make_literal(std::int64_t{0}));
    check(has_code(tab, "TESSERA_IR_SPAN"), "a span rejects a non-span prop");

    ir::IrModule event = make_module();
    event.components[0].root.children[0].events.emplace("onPointerDown", "bind::x");
    check(has_code(event, "TESSERA_IR_SPAN"), "a span supports onClick only");

    ir::IrModule ok = make_module();
    ir::Node& span_node = ok.components[0].root.children[0];
    span_node.props.emplace("role", ir::make_literal(std::string("link")));
    span_node.props.emplace("className", ir::make_literal(std::string("link")));
    span_node.props.emplace("aria-label", ir::make_literal(std::string("Terms")));
    span_node.style.emplace("color", ir::make_literal(std::string("#0000ff")));
    span_node.style.emplace("font-weight", ir::make_literal(std::string("700")));
    check(codes_of(ok).empty(),
          "span role/className/aria/colour/weight are valid: " + codes_of(ok));
}

void test_caps() {
    ir::IrModule many = make_module(17);
    check(has_code(many, "TESSERA_IR_SPAN"), "more than 16 spans is rejected");

    ir::IrModule long_text = make_module(1);
    long_text.components[0].root.children[0].text = ir::make_literal(std::string(300, 'x'));
    check(has_code(long_text, "TESSERA_IR_SPAN"), "more than 256 span bytes is rejected");
}

} // namespace

int main() {
    test_valid_spans();
    test_parent_text_rules();
    test_span_text_rules();
    test_span_attributes();
    test_caps();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
