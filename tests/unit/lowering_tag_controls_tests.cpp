// SPDX-License-Identifier: MIT

// Tags that lower onto library components (ADR-059 group E, issue #1041): `input type="checkbox"`,
// `"radio"` and `"range"`, `progress`, `meter` and `details`/`summary` resolve to the component the
// file imports under the library name, with their attributes rewritten onto its props. The stubs
// below declare the same props as packages/tessera-ui; the gallery build compiles the real ones.

#include "lowering_module_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kLibrary =
    "import { Children } from \"tessera\";\n"
    "export type CheckboxProps = { label: string; checked?: boolean; onToggle: () => void;\n"
    "  disabled?: boolean; children: Children };\n"
    "export function Checkbox({ label, checked = false, onToggle, disabled = false, children }:\n"
    "  CheckboxProps) { return <View role=\"checkbox\" tabIndex={0} aria-label={label}>{children}"
    "</View>; }\n"
    "export type RadioProps = { label: string; selected?: boolean; onSelect: () => void;\n"
    "  disabled?: boolean; children: Children };\n"
    "export function Radio({ label, selected = false, onSelect, disabled = false, children }:\n"
    "  RadioProps) { return <View role=\"radio\" tabIndex={0} "
    "aria-label={label}>{children}</View>; }\n"
    "export type RangeProps = { label: string; min?: int; max?: int; step?: int;\n"
    "  defaultValue?: int; children: Children };\n"
    "export function Range({ label, min = 0, max = 100, step = 1, defaultValue = 0,\n"
    "  children }: RangeProps) { return <View role=\"slider\" tabIndex={0} aria-label={label}>"
    "{children}</View>; }\n"
    "export type ProgressBarProps = { label: string; value?: int; max?: int; children: Children "
    "};\n"
    "export function ProgressBar({ label, value = 0, max = 100, children }: ProgressBarProps) {\n"
    "  return <View role=\"progressbar\" aria-label={label}>{children}</View>; }\n"
    "export type MeterProps = { label: string; value?: int; min?: int; max?: int; low?: int;\n"
    "  high?: int; optimum?: int; children: Children };\n"
    "export function Meter({ label, value = 0, min = 0, max = 100, low = 0, high = 100, optimum = "
    "50,\n"
    "  children }: MeterProps) { return <View role=\"meter\" aria-label={label}>{children}</View>; "
    "}\n"
    "export type DetailsProps = { summary: string; defaultOpen?: boolean; children: Children "
    "};\n"
    "export function Details({ summary, defaultOpen = false, children }: DetailsProps) {\n"
    "  return <View aria-label={summary}>{children}</View>; }\n";

struct Lowered {
    Project project;
    lowering::ModuleLoweringResult result;
    const ir::Node* root = nullptr;
};

// `imports` is the import line of the page; `body` the JSX inside its root View.
std::unique_ptr<Lowered> lower_page(std::string_view imports, std::string_view body) {
    auto lowered = std::make_unique<Lowered>();
    lowered->project.add(parse_file("ui/lib.tsx", std::string(kLibrary)));
    lowered->project.add(parse_file(
        "ui/Page.tsx",
        "import { useState } from \"tessera\";\n" + std::string(imports) +
            "\nexport function Page() {\n  const [on, setOn] = useState<boolean>(false);\n"
            "  return <View>" +
            std::string(body) + "</View>;\n}\n"));
    lowered->result = lowered->project.lower("app");
    if (lowered->result) {
        if (const ir::Component* page = find_component(*lowered->result, "Page")) {
            lowered->root = &page->root;
        }
    }
    return lowered;
}

std::string prop(const ir::Node& node, const char* name) {
    const auto found = node.props.find(name);
    return found == node.props.end() || !found->second ? "<absent>"
                                                       : expr_text(found->second.get());
}

constexpr const char* kAllImports =
    "import { Checkbox, Radio, Range, ProgressBar, Meter, Details } from \"./lib\";";

void test_checkbox() {
    const auto page =
        lower_page(kAllImports, "<input type=\"checkbox\" checked={on} aria-label=\"Accept\" "
                                "disabled onChange={() => setOn(!on)} className=\"x\" />");
    check(page->result.has_value(), "a checkbox input lowers: " + first_message(page->result));
    if (page->root == nullptr || page->root->children.size() != 1) {
        return;
    }
    const ir::Node& node = page->root->children[0];
    check_eq(node.component, "comp::Checkbox", "the input is the imported Checkbox");
    check_eq(prop(node, "label"), "{\"kind\":\"literal\",\"value\":\"Accept\"}",
             "aria-label is the label prop");
    check(node.props.count("checked") == 1, "checked stays checked");
    check_eq(prop(node, "disabled"), "{\"kind\":\"literal\",\"value\":true}", "disabled flag");
    check(node.events.count("onToggle") == 1 && node.events.count("onChange") == 0,
          "onChange is onToggle");
    check(node.props.count("type") == 0, "type is dropped");
    check(node.props.count("className") == 1, "className is kept");
}

void test_radio() {
    const auto page = lower_page(
        kAllImports,
        "<input type=\"radio\" checked={on} aria-label=\"One\" onChange={() => setOn(true)} />");
    check(page->result.has_value(), "a radio input lowers: " + first_message(page->result));
    if (page->root == nullptr || page->root->children.size() != 1) {
        return;
    }
    const ir::Node& node = page->root->children[0];
    check_eq(node.component, "comp::Radio", "the input is the imported Radio");
    check(node.props.count("selected") == 1 && node.props.count("checked") == 0,
          "checked is selected");
    check(node.events.count("onSelect") == 1, "onChange is onSelect");
}

void test_range() {
    const auto page = lower_page(kAllImports, "<input type=\"range\" min={0} max={10} step={2} "
                                              "defaultValue={4} aria-label=\"Volume\" />");
    check(page->result.has_value(), "a range input lowers: " + first_message(page->result));
    if (page->root == nullptr || page->root->children.size() != 1) {
        return;
    }
    const ir::Node& node = page->root->children[0];
    check_eq(node.component, "comp::Range", "the input is the imported Range");
    check_eq(prop(node, "max"), "{\"kind\":\"literal\",\"value\":10}", "max");
    check_eq(prop(node, "defaultValue"), "{\"kind\":\"literal\",\"value\":4}", "defaultValue");
    check_eq(prop(node, "label"), "{\"kind\":\"literal\",\"value\":\"Volume\"}", "label");
}

void test_progress_and_meter() {
    const auto page = lower_page(
        kAllImports,
        "<progress value={30} max={80} aria-label=\"Upload\" />"
        "<meter value={5} min={0} max={10} low={3} high={8} optimum={9} aria-label=\"Disk\" />");
    check(page->result.has_value(), "progress and meter lower: " + first_message(page->result));
    if (page->root == nullptr || page->root->children.size() != 2) {
        return;
    }
    check_eq(page->root->children[0].component, "comp::ProgressBar", "progress is ProgressBar");
    check_eq(prop(page->root->children[0], "value"), "{\"kind\":\"literal\",\"value\":30}",
             "value");
    check_eq(page->root->children[1].component, "comp::Meter", "meter is Meter");
    check_eq(prop(page->root->children[1], "optimum"), "{\"kind\":\"literal\",\"value\":9}",
             "optimum");
}

void test_details() {
    const auto page =
        lower_page(kAllImports, "<details open><summary>More</summary><Text>Body</Text></details>");
    check(page->result.has_value(), "details lowers: " + first_message(page->result));
    if (page->root == nullptr || page->root->children.size() != 1) {
        return;
    }
    const ir::Node& node = page->root->children[0];
    check_eq(node.component, "comp::Details", "details is Details");
    check_eq(prop(node, "summary"), "{\"kind\":\"literal\",\"value\":\"More\"}", "summary text");
    check_eq(prop(node, "defaultOpen"), "{\"kind\":\"literal\",\"value\":true}",
             "open is defaultOpen");
    check(node.children.size() == 1, "the summary is not slot content");
}

void test_text_input_is_unchanged() {
    const auto page = lower_page(kAllImports, "<input placeholder=\"Name\" />");
    check(page->result.has_value(), "a text input still lowers: " + first_message(page->result));
    if (page->root != nullptr && page->root->children.size() == 1) {
        check(page->root->children[0].component.empty(), "a text input is not a component");
    }
}

void test_missing_import_is_named() {
    const auto page =
        lower_page("", "<input type=\"checkbox\" aria-label=\"x\" onChange={() => setOn(!on)} />");
    check(!page->result.has_value(), "a checkbox without the import fails");
    check_eq(first_code(page->result), "TESSERA_IR_UNRESOLVED", "the code");
    check(!page->result && page->result.error().front().suggestion.find("tessera-ui/Checkbox") !=
                               std::string::npos,
          "the suggestion names the import");
}

void test_rejected_attributes() {
    const auto name =
        lower_page(kAllImports, "<input type=\"checkbox\" name=\"n\" aria-label=\"x\" />");
    check_eq(first_code(name->result), "TESSERA_JSX_SUBSET_HTML_ATTR", "name is rejected");
    const auto uncontrolled =
        lower_page(kAllImports, "<input type=\"checkbox\" defaultChecked aria-label=\"x\" />");
    check_eq(first_code(uncontrolled->result), "TESSERA_JSX_SUBSET_HTML_ATTR",
             "defaultChecked is rejected");
    const auto controlled =
        lower_page(kAllImports, "<input type=\"range\" value={3} aria-label=\"x\" />");
    check_eq(first_code(controlled->result), "TESSERA_JSX_SUBSET_HTML_ATTR",
             "a controlled range is rejected until #1051");
    const auto unknown = lower_page(kAllImports, "<progress value={1} low={2} />");
    check_eq(first_code(unknown->result), "TESSERA_JSX_SUBSET_HTML_ATTR",
             "an attribute the component lacks is rejected");
}

void test_details_shape() {
    const auto stray = lower_page(kAllImports, "<summary>Alone</summary>");
    check_eq(first_code(stray->result), "TESSERA_IR_UNSUPPORTED", "a stray summary is rejected");
    const auto none = lower_page(kAllImports, "<details><Text>Body</Text></details>");
    check_eq(first_code(none->result), "TESSERA_IR_UNSUPPORTED", "details needs a summary");
    const auto rich = lower_page(kAllImports, "<details><summary><b>Bold</b></summary></details>");
    check_eq(first_code(rich->result), "TESSERA_IR_UNSUPPORTED",
             "a summary holds static text only");
}

} // namespace

int main() {
    test_checkbox();
    test_radio();
    test_range();
    test_progress_and_meter();
    test_details();
    test_text_input_is_unchanged();
    test_missing_import_is_named();
    test_rejected_attributes();
    test_details_shape();
    return finish("lowering tag controls tests");
}
