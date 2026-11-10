// SPDX-License-Identifier: MIT

// Slice 9 rejection cases: unknown component tags, component references in attribute position,
// component prop forms that need the type table, duplicate component names, and project import
// resolution failures (unknown file, unknown name, cycle, duplicate file).

#include "lowering_module_test_util.hpp"

namespace {

using namespace lowering_module_test;

constexpr std::string_view kCard = "function Card(props: P) { return <View />; }\n";

void test_unknown_component_tag() {
    const auto fixture = parse_file("ui/App.tsx", "function App() { return <Missing />; }\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(!result.has_value(), "an unknown tag is rejected");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
    check(first_message(result).find("Missing") != std::string::npos,
          "the diagnostic names the tag");
}

void test_component_in_attribute_position() {
    const auto fixture =
        parse_file("ui/App.tsx", std::string(kCard) +
                                     "function Other(props: P) { return <View />; }\n"
                                     "function App() { return <Card title={<Other />} />; }\n");
    const auto source = fixture->module_source();
    const auto result = lowering::lower_module(source, "m");
    check(!result.has_value(), "a component reference is not a prop value");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto style =
        parse_file("ui/App2.tsx", std::string(kCard) + "function App() { return <Card style={{ "
                                                       "width: 1 }} />; }\n");
    const auto style_result = lowering::lower_module(style->module_source(), "m");
    check(!style_result.has_value(), "a style attribute on a component needs its prop type");
    check_eq(first_code(style_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_component_callback_forms() {
    const auto parameter =
        parse_file("ui/App.tsx", std::string(kCard) + "function App() {\n"
                                                      "  const [n, setN] = useState<int>(0);\n"
                                                      "  return <Card onPick={(e) => setN(1)} />;\n"
                                                      "}\n");
    const auto parameter_result = lowering::lower_module(parameter->module_source(), "m");
    check(!parameter_result.has_value(), "a callback parameter needs the callback prop type");
    check_eq(first_code(parameter_result), "TESSERA_IR_UNSUPPORTED", "the code");

    const auto setter =
        parse_file("ui/App.tsx", std::string(kCard) + "function App() {\n"
                                                      "  const [n, setN] = useState<int>(0);\n"
                                                      "  return <Card onPick={setN} />;\n"
                                                      "}\n");
    const auto setter_result = lowering::lower_module(setter->module_source(), "m");
    check(!setter_result.has_value(), "a setter reference needs the callback payload type");
    check_eq(first_code(setter_result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_component_props_spread() {
    const auto fixture =
        parse_file("ui/App.tsx",
                   std::string(kCard) + "function App(props: P) { return <Card {...props} />; }\n");
    const auto result = lowering::lower_module(fixture->module_source(), "m");
    check(!result.has_value(), "a props spread on a component needs the props type");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
}

void test_duplicate_component_names() {
    auto first = parse_file("ui/A.tsx", std::string(kCard));
    auto second = parse_file("ui/B.tsx", std::string(kCard));
    Project project;
    project.add(std::move(first));
    project.add(std::move(second));
    const auto result = project.lower("m");
    check(!result.has_value(), "a component name is project-wide unique (decision 3)");
    check_eq(first_code(result), "TESSERA_IR_DUPLICATE_ID", "the code");
}

void test_import_failures() {
    auto missing_file = parse_file("ui/App.tsx", "import { Card } from \"./Missing\";\n"
                                                 "function App() { return <Card />; }\n");
    Project missing_project;
    missing_project.add(std::move(missing_file));
    const auto missing_result = missing_project.lower("m");
    check(!missing_result.has_value(), "an import of an unknown file is rejected");
    check_eq(first_code(missing_result), "TESSERA_IR_UNRESOLVED", "the code");

    auto card = parse_file("ui/Card.tsx", std::string(kCard));
    auto unknown_name = parse_file("ui/App.tsx", "import { Nope } from \"./Card\";\n"
                                                 "function App() { return <View />; }\n");
    Project name_project;
    name_project.add(std::move(card));
    name_project.add(std::move(unknown_name));
    const auto name_result = name_project.lower("m");
    check(!name_result.has_value(), "an import of an unknown name is rejected");
    check_eq(first_code(name_result), "TESSERA_IR_UNRESOLVED", "the code");

    auto a = parse_file("ui/A.tsx", "import { B } from \"./B\";\n"
                                    "export function A(props: P) { return <B />; }\n");
    auto b = parse_file("ui/B.tsx", "import { A } from \"./A\";\n"
                                    "export function B(props: P) { return <A />; }\n");
    Project cycle_project;
    cycle_project.add(std::move(a));
    cycle_project.add(std::move(b));
    const auto cycle_result = cycle_project.lower("m");
    check(!cycle_result.has_value(), "an import cycle is rejected");
    check_eq(first_code(cycle_result), "TESSERA_IR_UNSUPPORTED", "the code");
    check(first_message(cycle_result).find("cycle") != std::string::npos,
          "the diagnostic says cycle");
}

// Section 3.4: the slot is a child position, not a value, an attribute or a repeated node.
void test_children_slot_rejections() {
    const auto reject = [](std::string_view what, std::string_view source, std::string_view code) {
        const auto fixture = parse_file("ui/App.tsx", std::string(source));
        const auto result = lowering::lower_module(fixture->module_source(), "m");
        check(!result.has_value(), std::string(what) + " is rejected");
        check_eq(first_code(result), std::string(code), std::string(what) + " code");
    };

    reject("a Slot in a Text",
           "function Card({ children }: P) { return <Text>{children}</Text>; }\n",
           "TESSERA_IR_UNSUPPORTED");
    reject("`props.children` of a non-destructured parameter",
           "function Card(props: P) { return <View>{props.children}</View>; }\n",
           "TESSERA_IR_SLOT");
    reject("a children attribute",
           "function Card(props: P) { return <View />; }\n"
           "function App() { return <Card children={<View />} />; }\n",
           "TESSERA_IR_SLOT");
    reject("a default value on the children prop",
           "function Card({ children = 1 }: P) { return <View>{children}</View>; }\n",
           "TESSERA_IR_SLOT");
    reject("a Slot inside slot content",
           "function Card({ children }: P) { return <View>{children}</View>; }\n"
           "function App({ children }: P) { return <Card>{children}</Card>; }\n",
           "TESSERA_IR_SLOT");
    reject("two Slots in one component",
           "function Card({ children }: P) { return <View>{children}{children}</View>; }\n",
           "TESSERA_IR_SLOT");
    reject("a Slot inside a loop template",
           "function Card({ children }: P) {\n"
           "  const [rows, setRows] = useState<int[]>([]);\n"
           "  return <View>{rows.map((row) => <View key={row}>{children}</View>)}</View>;\n"
           "}\n",
           "TESSERA_IR_SLOT");
}

void test_module_single_file_relative_import() {
    const auto fixture = parse_file("ui/App.tsx", "import { Card } from \"./Card\";\n"
                                                  "function App() { return <Card />; }\n");
    const auto result = lowering::lower_module(fixture->module_source(), "m");
    check(!result.has_value(), "lower_module cannot resolve another file");
    check_eq(first_code(result), "TESSERA_IR_UNRESOLVED", "the code");
}

void test_duplicate_file_paths() {
    auto first = parse_file("ui/App.tsx", "function App() { return <View />; }\n");
    auto second = parse_file("ui/App.tsx", "function Other() { return <View />; }\n");
    Project project;
    project.add(std::move(first));
    project.add(std::move(second));
    const auto result = project.lower("m");
    check(!result.has_value(), "the same file cannot be lowered twice");
    check_eq(first_code(result), "TESSERA_IR_MALFORMED", "the code");
}

void test_first_error_only() {
    const auto fixture = parse_file("ui/App.tsx", "function App() {\n"
                                                  "  return <View><Missing /><Other /></View>;\n"
                                                  "}\n");
    const auto result = lowering::lower_module(fixture->module_source(), "m");
    check(!result.has_value(), "a module with errors yields no IR");
    if (!result.has_value()) {
        check(result.error().size() == 1, "only the first error is reported");
        check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the first error wins");
    }
}

} // namespace

int main() {
    test_unknown_component_tag();
    test_component_in_attribute_position();
    test_component_callback_forms();
    test_component_props_spread();
    test_children_slot_rejections();
    test_duplicate_component_names();
    test_import_failures();
    test_module_single_file_relative_import();
    test_duplicate_file_paths();
    test_first_error_only();
    return finish("lowering module reject tests");
}
