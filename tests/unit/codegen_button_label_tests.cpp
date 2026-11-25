// SPDX-License-Identifier: MIT

// A `<Button>` whose children are only text carries that text as `props.label` (elements-basic.md
// section 1). The native generator painted `node.text` alone, so such a button was built with no
// caption: the scaffolded counter template drew two empty boxes, while the design-time preview
// (which reads `label`) showed them. Hand-built IR, one construct at a time:
//
// * a static label is set as the button's paint payload, like a `Text`'s literal,
// * an interpolated label is repainted by apply_state(), like an interpolated `Text`,
// * the binding read set names the cells the label reads, so a state change re-runs the update,
// * a button with element children has no label and so no caption payload.

#include "binding_reads.hpp"
#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace codegen = tessera::compiler::codegen;
namespace internal = tessera::compiler::codegen::internal;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

ir::SourceSpan span() {
    ir::SourceSpan value;
    value.file = "ui/Caption.tsx";
    value.offset = 0;
    value.length = 10;
    value.line = 1;
    value.column = 1;
    value.end_line = 1;
    value.end_column = 11;
    return value;
}

ir::Node node_of(ir::NodeKind kind, std::string id, std::string ui_id) {
    ir::Node node;
    node.source = span();
    node.kind = kind;
    node.id = std::move(id);
    node.ui_id = std::move(ui_id);
    return node;
}

ir::Node button(std::string id, std::string ui_id, ir::ExprPtr label) {
    ir::Node node = node_of(ir::NodeKind::Button, std::move(id), std::move(ui_id));
    if (label) {
        node.props["label"] = std::move(label);
    }
    return node;
}

ir::ExprPtr count_label() {
    std::vector<ir::TemplatePart> parts;
    parts.push_back(ir::text_part("Count: "));
    parts.push_back(ir::expr_part(ir::make_ref("state::count")));
    return ir::make_template(std::move(parts));
}

ir::IrModule make_module() {
    ir::IrModule module;
    module.name = "caption";
    ir::Component component;
    component.id = "comp::Caption";
    component.component_id = "71";
    component.name = "Caption";
    component.source = span();
    component.state.push_back({"state::count", "count", "int", ir::make_literal(std::int64_t{0})});
    component.root = node_of(ir::NodeKind::View, "node::1", "501");
    component.root.children.push_back(
        button("node::2", "502", ir::make_literal(std::string("Add"))));
    component.root.children.push_back(button("node::3", "503", count_label()));
    // An element child: the lowering keeps the children and sets no label.
    ir::Node composite = button("node::4", "504", nullptr);
    ir::Node inner = node_of(ir::NodeKind::Text, "node::5", "505");
    inner.text = ir::make_literal(std::string("inner"));
    composite.children.push_back(std::move(inner));
    component.root.children.push_back(std::move(composite));
    module.components.push_back(std::move(component));
    return module;
}

std::string base_header() {
    const ir::IrModule module = make_module();
    const auto result = codegen::generate(module, {}, std::nullopt);
    if (!result) {
        return "error: " +
               (result.error().empty() ? std::string("<empty>") : result.error().front());
    }
    for (const codegen::GeneratedFile& file : *result) {
        if (file.path == "Caption.g.hpp") {
            return file.text;
        }
    }
    return {};
}

void test_fixture_is_valid() {
    std::string codes;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(make_module())) {
        codes += diagnostic.code + ": " + diagnostic.message + "; ";
    }
    check(codes.empty(), "the caption fixture is a valid IR module: " + codes);
}

void test_static_label_is_painted() {
    const std::string header = base_header();
    check(!header.starts_with("error:"), header);
    check(contains(header, ".text = \"Add\";"), "a static label becomes the button's paint text");
    check(contains(header, "set_paint_data(std::move(paint_"),
          "the static label is installed as the node's paint payload");
}

void test_interpolated_label_is_repainted() {
    const std::string header = base_header();
    check(contains(header, "text_node_3_"),
          "an interpolated label has an update slot, like an interpolated Text");
    check(contains(header, "\"Count: \"") || contains(header, "Count: "),
          "the update formats the label's text part");
}

void test_label_joins_the_read_set() {
    const ir::IrModule module = make_module();
    const internal::BindingReadTable table =
        internal::analyze_binding_reads(module.components.front());
    // Pre-order ordinals: View 1, then the three buttons 2, 3, 4.
    const internal::ReadSet* text = table.find(internal::BindingKind::Text, 3);
    check(text != nullptr && !text->all && text->cells == std::vector<std::string>{"state::count"},
          "the interpolated caption is a Text binding that reads exactly the count cell");
    check(table.find(internal::BindingKind::Attribute, 3, "label") == nullptr,
          "the caption is not also an attribute binding");
    check(table.find(internal::BindingKind::Text, 4) == nullptr,
          "a button with element children has no caption binding");
}

void test_a_button_with_children_has_no_caption() {
    const std::string header = base_header();
    check(!contains(header, "text_node_4_"), "a button with element children has no caption slot");
}

} // namespace

int main() {
    test_fixture_is_valid();
    test_static_label_is_painted();
    test_interpolated_label_is_repainted();
    test_label_joins_the_read_set();
    test_a_button_with_children_has_no_caption();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
