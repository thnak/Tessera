// SPDX-License-Identifier: MIT

// An imported component shadows an intrinsic element of the same name in that file (issue #291,
// docs/specs/jsx-subset-v1.md section 4): `import { Button } from "./Button"` makes `<Button>` the
// component, a file without the import keeps the intrinsic, and a component declared in the same
// file does not shadow.

#include "lowering_module_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kButton = "export type ButtonProps = { label: string };\n"
                                     "export function Button({ label }: ButtonProps) {\n"
                                     "  return <View role=\"button\" aria-label={label} />;\n"
                                     "}\n";

void test_import_shadows_the_intrinsic() {
    Project project;
    project.add(parse_file("ui/Button.tsx", std::string(kButton)));
    project.add(parse_file("ui/App.tsx", "import { Button } from \"./Button\";\n"
                                         "function App() {\n"
                                         "  return <View><Button label=\"Save\" /></View>;\n"
                                         "}\n"));
    const auto result = project.lower("shadow");
    check(result.has_value(), "the import lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr && app->root.children.size() == 1, "App has one child");
    if (app != nullptr && app->root.children.size() == 1) {
        const ir::Node& node = app->root.children[0];
        check(node.kind == ir::NodeKind::Component && node.component == "comp::Button",
              "`<Button>` is the imported component");
    }
}

void test_renamed_import_shadows_under_its_local_name() {
    Project project;
    project.add(parse_file("ui/Button.tsx", std::string(kButton)));
    project.add(parse_file("ui/App.tsx",
                           "import { Button as Action } from \"./Button\";\n"
                           "function App() {\n"
                           "  return <View><Action label=\"Save\" /><Button>x</Button></View>;\n"
                           "}\n"));
    const auto result = project.lower("shadow");
    check(result.has_value(), "the renamed import lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr && app->root.children.size() == 2, "both children lowered");
    if (app != nullptr && app->root.children.size() == 2) {
        check(app->root.children[0].kind == ir::NodeKind::Component, "`<Action>` is a component");
        check(app->root.children[1].kind != ir::NodeKind::Component,
              "`<Button>` stays the intrinsic when only `Action` was imported");
    }
}

void test_a_file_without_the_import_keeps_the_intrinsic() {
    Project project;
    project.add(parse_file("ui/Button.tsx", std::string(kButton)));
    project.add(parse_file("ui/Other.tsx", "function Other() {\n"
                                           "  return <Button>go</Button>;\n"
                                           "}\n"));
    const auto result = project.lower("shadow");
    check(result.has_value(), "the plain file lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* other = find_component(*result, "Other");
    check(other != nullptr && other->root.kind != ir::NodeKind::Component,
          "without the import `<Button>` is the intrinsic element");
}

void test_a_same_file_declaration_does_not_shadow() {
    Project project;
    project.add(parse_file("ui/App.tsx", "function Button() { return <View />; }\n"
                                         "function App() { return <Button>x</Button>; }\n"));
    const auto result = project.lower("shadow");
    check(result.has_value(), "the file lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* app = find_component(*result, "App");
    check(app != nullptr && app->root.kind != ir::NodeKind::Component,
          "a component declared in the same file does not shadow the intrinsic");
}

} // namespace

int main() {
    test_import_shadows_the_intrinsic();
    test_renamed_import_shadows_under_its_local_name();
    test_a_file_without_the_import_keeps_the_intrinsic();
    test_a_same_file_declaration_does_not_shadow();
    return jsx_test::finish("lowering import shadow");
}
