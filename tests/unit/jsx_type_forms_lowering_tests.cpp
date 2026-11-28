// SPDX-License-Identifier: MIT

// ADR-058 typing rule 1 (#547): the TypeScript spellings a port writes lower to the same IR as the
// plain forms they stand for, so the IR (irVersion 1) and both codegen targets, which read only the
// IR, are unchanged. Each case lowers a component twice and compares the component text.

#include "lowering_module_test_util.hpp"

#include <iostream>
#include <string>

namespace {

using namespace lowering_module_test;

std::string lowered(const std::string& source) {
    const auto fixture = parse_file("ui/Page.tsx", source);
    const auto result = lowering::lower_module(fixture->module_source(), "demo");
    check(result.has_value(), "the component lowers");
    if (!result) {
        std::cerr << "  error: " << first_message(result) << "\n";
        return {};
    }
    const ir::Component* page = find_component(*result, "Page");
    check(page != nullptr, "the Page component is present");
    if (page == nullptr) {
        return {};
    }
    // Source spans differ with the length of the declarations above the component; the IR does not.
    std::string text = describe_component(*page);
    const auto cut = text.find("\"source\"");
    return cut == std::string::npos ? text : text.substr(0, cut);
}

const std::string kBody =
    "function Page(props: Props) {\n"
    "  const [maybe, setMaybe] = useState<string | null>(null);\n"
    "  return <View><Text>{maybe ?? props.label}</Text><Text>{props.tone}</Text></View>;\n"
    "}\n";
const std::string kImports = "import { View, Text, useState } from \"tessera\";\n";

void test_sugar_lowers_to_the_plain_form() {
    const std::string sugar = kImports +
                              "enum Tone { Warm = \"warm\", Cool = \"cool\" }\n"
                              "const THEMES = { dark: \"d\", light: \"l\" } as const;\n"
                              "type Theme = typeof THEMES[keyof typeof THEMES];\n"
                              "interface Base { label: string; tone: Tone; extra: int }\n"
                              "type Props = Omit<Partial<Base>, \"extra\"> & { theme: Theme };\n" +
                              kBody;
    const std::string plain = kImports +
                              "type Tone = \"warm\" | \"cool\";\n"
                              "type Theme = \"d\" | \"l\";\n"
                              "type Props = { label?: string; tone?: Tone; theme: Theme };\n" +
                              kBody;
    const std::string a = lowered(sugar);
    check(!a.empty(), "the sugar form lowers to a component");
    check_eq(a, lowered(plain),
             "interface, enum, as const, utility types and ?? lower to the plain IR");
}

void test_rejections_carry_the_fix() {
    const auto fixture = parse_file_raw("ui/Bad.tsx", "interface P extends Q { a: int }\n");
    check(!fixture->parsed.ok(), "interface extends is rejected");
    check(fixture->parsed.diagnostics.front().suggestion.find("members") != std::string::npos,
          "the diagnostic carries the fix");
}

} // namespace

int main() {
    test_sugar_lowers_to_the_plain_form();
    test_rejections_carry_the_fix();
    return finish("jsx type forms lowering tests");
}
