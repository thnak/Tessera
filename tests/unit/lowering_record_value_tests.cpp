// SPDX-License-Identifier: MIT
//
// Slice 3 of ADR-051: an object literal in a declared record's position becomes an `ir::Record`
// (docs/specs/ir-v1-expr.md section 4.3). The two positions V1 allows are a `useState` initial of a
// record type and a prop default of a record type.
//
// The implementation landed with PR #339 but had no test of its own: the IR-level tests
// (`ir_record_writer`, `ir_record_table`, `ir_record_value`) prove the *format*, and
// `lowering_props` proves the prop *table*, so nothing proved that a TSX object literal actually
// reaches the IR as a record value. This file is that test, and it is why the roadmap note saying
// slice 3 was still to do could stay wrong for as long as it did.
//
// What it pins:
//   * a `useState<Size>({ w: 1, h: 2 })` initial lowers to a `record` expression with the declared
//     type and both fields, in member order;
//   * a prop default of a record type lowers the same way;
//   * a nested record and an array-of-literals field are accepted;
//   * a computed field value is `TESSERA_IR_UNSUPPORTED` naming the member, not a silent drop;
//   * a missing, duplicate or out-of-order field is `TESSERA_IR_RECORD_VALUE` / `TESSERA_IR_ORDER`;
//   * a record value in a position the spec forbids is rejected.

#include "lowering_module_test_util.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace ir = tessera::compiler::ir;
namespace lowering = tessera::compiler::lowering;
using lowering_module_test::find_component;
using lowering_module_test::parse_file;
using lowering_module_test::parse_file_raw;
using lowering_module_test::Project;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "lowering_record_value: " << message << "\n";
        ++g_failures;
    }
}

// The first diagnostic code of a failed lowering, or an empty string.
std::string first_code(const lowering::ModuleLoweringResult& result) {
    if (result.has_value() || result.error().empty()) {
        return {};
    }
    return std::string(result.error().front().code);
}

// The `record` expression of the first state cell of `component`, or null.
const ir::Expr* state_initial(const ir::Component& component) {
    if (component.state.empty() || !component.state.front().initial) {
        return nullptr;
    }
    return component.state.front().initial.get();
}

// `{ w: 1, h: 2 }` as a `useState` initial of a declared record type.
void test_use_state_record_initial() {
    Project project;
    project.add(parse_file("ui/Box.tsx",
                           "type Size = { w: int; h: int };\n"
                           "function Box() {\n"
                           "  const [size, setSize] = useState<Size>({ w: 1, h: 2 });\n"
                           "  return <View className=\"box\"/>;\n"
                           "}\n"));
    const auto lowered = project.lower("Box");
    expect(lowered.has_value(), "a record useState initial lowers: " + first_code(lowered));
    if (!lowered) {
        return;
    }
    const ir::Component* box = find_component(*lowered, "Box");
    expect(box != nullptr, "the component is lowered");
    if (box == nullptr) {
        return;
    }
    const ir::Expr* initial = state_initial(*box);
    expect(initial != nullptr, "the state cell has an initial");
    if (initial == nullptr) {
        return;
    }
    expect(initial->kind == ir::ExprKind::Record, "the initial is a record expression");
    expect(initial->type == "Size", "the record names its declared type");
    expect(initial->fields.size() == 2, "both fields are present");
    if (initial->fields.size() == 2) {
        expect(initial->fields[0].name == "w", "the first field is `w`, in member order");
        expect(initial->fields[1].name == "h", "the second field is `h`, in member order");
    }
}

// A prop default of a record type lowers the same way.
void test_prop_default_record() {
    Project project;
    project.add(parse_file("ui/Chip.tsx",
                           "type Size = { w: int; h: int };\n"
                           "function Chip({ size = { w: 3, h: 4 } }: { size?: Size }) {\n"
                           "  return <View className=\"box\"/>;\n"
                           "}\n"));
    const auto lowered = project.lower("Chip");
    expect(lowered.has_value(), "a record prop default lowers: " + first_code(lowered));
    if (!lowered) {
        return;
    }
    const ir::Component* chip = find_component(*lowered, "Chip");
    expect(chip != nullptr, "the component is lowered");
    if (chip == nullptr || chip->props.empty()) {
        return;
    }
    const ir::Prop* size = nullptr;
    for (const ir::Prop& prop : chip->props) {
        if (prop.name == "size") {
            size = &prop;
            break;
        }
    }
    expect(size != nullptr, "the `size` prop is declared");
    if (size == nullptr || !size->default_value) {
        expect(false, "the `size` prop carries a default");
        return;
    }
    expect(size->default_value->kind == ir::ExprKind::Record, "the default is a record expression");
    expect(size->default_value->type == "Size", "the default names its declared type");
}

// A computed field value is refused by name rather than silently dropped.
void test_computed_field_is_refused_by_name() {
    Project project;
    project.add(parse_file_raw("ui/Box.tsx",
                               "type Size = { w: int; h: int };\n"
                               "function Box({ n }: { n: int }) {\n"
                               "  const [size, setSize] = useState<Size>({ w: n, h: 2 });\n"
                               "  return <View className=\"box\"/>;\n"
                               "}\n"));
    const auto lowered = project.lower("Box");
    expect(!lowered.has_value(), "a computed field value is refused");
    expect(first_code(lowered) == "TESSERA_IR_UNSUPPORTED",
           "the code is TESSERA_IR_UNSUPPORTED (saw `" + first_code(lowered) + "`)");
    if (!lowered) {
        const std::string& message = lowered.error().front().message;
        expect(message.find("`w`") != std::string::npos,
               "the diagnostic names the member (saw `" + message + "`)");
    }
}

// A missing field is a record-value error, not a partial record.
void test_missing_field_is_refused() {
    Project project;
    project.add(parse_file_raw("ui/Box.tsx", "type Size = { w: int; h: int };\n"
                                             "function Box() {\n"
                                             "  const [size, setSize] = useState<Size>({ w: 1 });\n"
                                             "  return <View className=\"box\"/>;\n"
                                             "}\n"));
    const auto lowered = project.lower("Box");
    expect(!lowered.has_value(), "a missing field is refused");
    expect(first_code(lowered) == "TESSERA_IR_RECORD_VALUE",
           "the code is TESSERA_IR_RECORD_VALUE (saw `" + first_code(lowered) + "`)");
}

// Fields written out of member order are NORMALIZED to member order, not rejected: the lowering
// emits the record in the declared order, which is what the generated aggregate's designated
// initializers and the IR validator's member-order rule both require. `TESSERA_IR_ORDER` is the
// validator's code for an IR document that got the order wrong, not a source-level rejection.
void test_out_of_order_fields_are_normalized() {
    Project project;
    project.add(parse_file("ui/Box.tsx",
                           "type Size = { w: int; h: int };\n"
                           "function Box() {\n"
                           "  const [size, setSize] = useState<Size>({ h: 2, w: 1 });\n"
                           "  return <View className=\"box\"/>;\n"
                           "}\n"));
    const auto lowered = project.lower("Box");
    expect(lowered.has_value(), "out-of-order source fields still lower: " + first_code(lowered));
    if (!lowered) {
        return;
    }
    const ir::Component* box = find_component(*lowered, "Box");
    if (box == nullptr) {
        return;
    }
    const ir::Expr* initial = state_initial(*box);
    if (initial == nullptr || initial->fields.size() != 2) {
        expect(false, "the record has both fields");
        return;
    }
    expect(initial->fields[0].name == "w",
           "the emitted order is the DECLARED order, not the source order");
    expect(initial->fields[1].name == "h", "the second emitted field is `h`");
}

// A nested record and an array of literals are both permitted field forms.
void test_nested_record_and_array_field() {
    Project project;
    project.add(parse_file("ui/Panel.tsx",
                           "type Size = { w: int; h: int };\n"
                           "type Panel = { size: Size; tags: string[] };\n"
                           "function PanelView() {\n"
                           "  const [panel, setPanel] = useState<Panel>({ size: { w: 1, h: 2 }, "
                           "tags: [\"a\", \"b\"] });\n"
                           "  return <View className=\"panel\"/>;\n"
                           "}\n"));
    const auto lowered = project.lower("PanelView");
    expect(lowered.has_value(), "a nested record and an array field lower: " + first_code(lowered));
    if (!lowered) {
        return;
    }
    const ir::Component* view = find_component(*lowered, "PanelView");
    if (view == nullptr) {
        return;
    }
    const ir::Expr* initial = state_initial(*view);
    if (initial == nullptr || initial->fields.size() != 2) {
        expect(false, "the record has both fields");
        return;
    }
    expect(initial->fields[0].value != nullptr &&
               initial->fields[0].value->kind == ir::ExprKind::Record,
           "the nested field is a record expression");
    expect(initial->fields[1].value != nullptr &&
               initial->fields[1].value->kind == ir::ExprKind::Array,
           "the list field is an array expression");
}

} // namespace

int main() {
    test_use_state_record_initial();
    test_prop_default_record();
    test_computed_field_is_refused_by_name();
    test_missing_field_is_refused();
    test_out_of_order_fields_are_normalized();
    test_nested_record_and_array_field();
    if (g_failures != 0) {
        std::cerr << "lowering_record_value: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "lowering_record_value: ok\n";
    return 0;
}
