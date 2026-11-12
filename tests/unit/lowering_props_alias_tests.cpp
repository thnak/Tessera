// SPDX-License-Identifier: MIT

// Slice 4 (cross-file): a component whose props type is a type alias declared in another source
// file lowers to the same props record as the same alias declared inline (issue #70, "cross-file
// props aliases"). Type-only imports, plain imports, `as` renames and re-export chains all resolve
// to the one declaration that owns the object body, so the `ir::Prop` list is byte-identical, in
// declaration order, and generated C++ does not change with where the alias is authored.
//
// Diagnostics stay loud: a missing exported alias, an import that names a non-type declaration, a
// cycle through type-alias imports and an unresolvable module each produce their own code, and no
// case silently lowers an empty props record.

#include "lowering_module_test_util.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace lowering_module_test;
namespace codegen = tessera::compiler::codegen;

constexpr std::string_view kPropsBody = "{\n"
                                        "  title: string;\n"
                                        "  count?: int;\n"
                                        "  tone: string | null;\n"
                                        "  tags?: string[];\n"
                                        "  children: Children;\n"
                                        "};\n";

constexpr std::string_view kExpectedProps = "title:string::-\n"
                                            "count:int:?:-\n"
                                            "tone:optional<string>::-\n"
                                            "tags:list<string>:?:-\n"
                                            "children:children::-\n";

// The component under test, without its props type declaration: the inline and imported fixtures
// differ only above this text, so node ids and every non-span field must match.
constexpr std::string_view kCardComponent =
    "export function Card({ title, count, tags, children }: CardProps) {\n"
    "  return <View><Text>{title}</Text>{children}</View>;\n"
    "}\n";

// Replaces each `"source": { ... }` value with `{}`, so two lowerings of the same component can be
// compared across files (the spans differ by the declaration/import prefix; nothing else may).
std::string normalize_spans(std::string_view text) {
    std::string out(text);
    std::size_t at = 0;
    while ((at = out.find("\"source\":", at)) != std::string::npos) {
        const std::size_t open = out.find('{', at);
        if (open == std::string::npos) {
            break;
        }
        std::size_t close = open;
        int depth = 0;
        do {
            if (out[close] == '{') {
                ++depth;
            } else if (out[close] == '}') {
                --depth;
            }
            ++close;
        } while (depth > 0 && close < out.size());
        out.replace(open, close - open, "{}");
        at = open + 2;
    }
    return out;
}

std::string props_text(const ir::Component& card) {
    std::string out;
    for (const ir::Prop& prop : card.props) {
        out += prop.name + ":" + prop.type + ":" + (prop.optional ? "?" : "") + ":";
        out += prop.default_value ? expr_text(prop.default_value.get()) : "-";
        out += "\n";
    }
    return out;
}

std::string component_text(const lowering::LoweredModule& module) {
    const ir::Component* card = find_component(module, "Card");
    return card == nullptr ? std::string("<no Card>") : normalize_spans(describe_component(*card));
}

// One project of `files` (path, text) lowered with `lower_project`.
lowering::ModuleLoweringResult
lower_files(std::initializer_list<std::pair<const char*, std::string>> files) {
    Project project;
    for (const auto& [path, text] : files) {
        project.add(parse_file(path, text));
    }
    return project.lower("demo");
}

void test_inline_and_imported_aliases_are_identical() {
    const auto inline_result =
        lower_files({{"ui/Card.tsx", "type CardProps = " + std::string(kPropsBody) +
                                         std::string(kCardComponent)}});
    check(inline_result.has_value(),
          "the inline-alias fixture lowers: " + first_message(inline_result));
    const auto imported_result =
        lower_files({{"ui/types.tsx", "export type CardProps = " + std::string(kPropsBody)},
                     {"ui/Card.tsx", "import type { CardProps } from \"./types\";\n" +
                                         std::string(kCardComponent)}});
    check(imported_result.has_value(),
          "the imported-alias fixture lowers: " + first_message(imported_result));
    if (!inline_result || !imported_result) {
        return;
    }
    const ir::Component* inline_card = find_component(*inline_result, "Card");
    const ir::Component* imported_card = find_component(*imported_result, "Card");
    check(inline_card != nullptr && imported_card != nullptr, "both fixtures declare Card");
    if (inline_card == nullptr || imported_card == nullptr) {
        return;
    }
    check_eq(props_text(*imported_card), std::string(kExpectedProps),
             "the imported alias lowers every member in declaration order");
    check_eq(props_text(*imported_card), props_text(*inline_card),
             "inline and imported aliases lower the same Prop list");
    check_eq(component_text(*imported_result), component_text(*inline_result),
             "the canonical component IR is identical once source spans are normalized");
    check(imported_card->props.size() == 5 && !imported_card->props.empty(),
          "the props record is never silently empty");
}

void test_import_forms_resolve() {
    const std::string body = "type CardProps = " + std::string(kPropsBody);
    const std::string component(kCardComponent);
    Project project;
    project.add(parse_file("ui/types.tsx", "export " + body));
    project.add(parse_file("ui/base.tsx", "export " + body));
    project.add(parse_file("ui/reexported.tsx", "export type { CardProps } from \"./base\";\n"));
    project.add(parse_file("ui/locally_reexported.tsx",
                           "import type { CardProps as Inner } from \"./base\";\nexport type { "
                           "Inner as CardProps };\n"));

    struct Case {
        const char* what;
        const char* import_line;
        const char* props_type;
    };
    const Case cases[] = {
        {"`import type`", "import type { CardProps } from \"./types\";\n", "CardProps"},
        {"plain import", "import { CardProps } from \"./types\";\n", "CardProps"},
        {"`as` rename", "import type { CardProps as Props } from \"./types\";\n", "Props"},
        {"re-export chain", "import type { CardProps } from \"./reexported\";\n", "CardProps"},
        {"local re-export rename", "import type { CardProps } from \"./locally_reexported\";\n",
         "CardProps"},
    };
    std::string canonical;
    for (const Case& test : cases) {
        const std::string body_text = "export function Card({ title, count, tags, children }: " +
                                      std::string(test.props_type) +
                                      ") {\n"
                                      "  return <View><Text>{title}</Text>{children}</View>;\n"
                                      "}\n";
        Project fixture;
        fixture.add(parse_file("ui/Card.tsx", std::string(test.import_line) + body_text));
        fixture.add(parse_file("ui/types.tsx", "export " + body));
        fixture.add(parse_file("ui/base.tsx", "export " + body));
        fixture.add(
            parse_file("ui/reexported.tsx", "export type { CardProps } from \"./base\";\n"));
        fixture.add(parse_file("ui/locally_reexported.tsx",
                               "import type { CardProps as Inner } from \"./base\";\n"
                               "export type { Inner as CardProps };\n"));
        const auto project_result = fixture.lower("demo");
        check(project_result.has_value(),
              std::string(test.what) + " resolves: " + first_message(project_result));
        if (!project_result) {
            continue;
        }
        const ir::Component* card = find_component(*project_result, "Card");
        check(card != nullptr, std::string(test.what) + " lowers Card");
        if (card == nullptr) {
            continue;
        }
        check_eq(props_text(*card), std::string(kExpectedProps),
                 std::string(test.what) + " lowers the same records");
        if (canonical.empty()) {
            canonical = component_text(*project_result);
        } else {
            check_eq(component_text(*project_result), canonical,
                     std::string(test.what) + " keeps the canonical IR identical");
        }
    }
}

void test_optional_slot_and_default_through_the_import() {
    const std::string body = "type BadgeProps = {\n"
                             "  label: string;\n"
                             "  count?: int;\n"
                             "  children: Children;\n"
                             "};\n";
    const std::string component =
        "export function Badge({ label = \"x\", count, children }: BadgeProps) {\n"
        "  return <View><Text>{label}</Text>{children}</View>;\n"
        "}\n";
    const auto inline_result = lower_files({{"ui/Badge.tsx", body + component}});
    const auto imported_result = lower_files(
        {{"ui/types.tsx", "export " + body},
         {"ui/Badge.tsx", "import type { BadgeProps } from \"./types\";\n" + component}});
    check(inline_result.has_value() && imported_result.has_value(),
          "both Badge fixtures lower: " + first_message(imported_result));
    if (!inline_result || !imported_result) {
        return;
    }
    const ir::Component* badge = find_component(*imported_result, "Badge");
    check(badge != nullptr, "the imported-alias Badge lowers");
    if (badge == nullptr) {
        return;
    }
    check_eq(props_text(*badge),
             "label:string::{\"kind\":\"literal\",\"value\":\"x\"}\ncount:int:?:-\n"
             "children:children::-\n",
             "the destructuring default, `?` and the children slot survive the import");
    check_eq(component_text(*imported_result), component_text(*inline_result),
             "optional/slot/default records match the inline-alias lowering");
}

// Reports the first code of a failing project, and proves no IR (and so no props record) came back.
std::string expect_error(std::initializer_list<std::pair<const char*, std::string>> files,
                         std::string_view what) {
    const auto result = lower_files(files);
    check(!result.has_value(), std::string(what) + " is rejected");
    if (result.has_value()) {
        return {};
    }
    return first_code(result);
}

void test_alias_resolution_errors_are_distinct() {
    const std::string missing_export = expect_error(
        {{"ui/types.tsx", "export type OtherProps = { title: string };\n"},
         {"ui/Card.tsx", "import type { CardProps } from \"./types\";\n"
                         "export function Card(props: CardProps) { return <View />; }\n"}},
        "a missing exported alias");
    check_eq(missing_export, "TESSERA_IR_TYPE_UNRESOLVED", "missing alias code");
    {
        const auto result = lower_files(
            {{"ui/types.tsx", "export type OtherProps = { title: string };\n"},
             {"ui/Card.tsx", "import type { CardProps } from \"./types\";\n"
                             "export function Card(props: CardProps) { return <View />; }\n"}});
        check(!result.has_value() && first_message(result).find("CardProps") != std::string::npos &&
                  first_message(result).find("./types") != std::string::npos,
              "the missing-alias diagnostic names the name and the module");
    }
    // A plain (value) import that names nothing on the target keeps the existing code.
    check_eq(expect_error({{"ui/types.tsx", "export type OtherProps = { title: string };\n"},
                           {"ui/Card.tsx", "import { CardProps } from \"./types\";\n"
                                           "export function Card(props: CardProps) { return "
                                           "<View />; }\n"}},
                          "a plain import of a missing name"),
             "TESSERA_IR_UNRESOLVED", "plain missing-name import code");

    const std::string non_type = expect_error(
        {{"ui/types.tsx", "export function CardProps() { return <View />; }\n"},
         {"ui/Card.tsx", "import { CardProps } from \"./types\";\n"
                         "export function Card(props: CardProps) { return <View />; }\n"}},
        "an import of a non-type declaration");
    check_eq(non_type, "TESSERA_IR_TYPE_NOT_ALIAS", "non-type props alias code");

    const std::string cycle = expect_error(
        {{"ui/a.tsx", "import type { BProps } from \"./b\";\n"
                      "export type AProps = BProps;\n"},
         {"ui/b.tsx", "import type { AProps } from \"./a\";\n"
                      "export type BProps = AProps;\n"},
         {"ui/Card.tsx", "import type { AProps } from \"./a\";\n"
                         "export function Card(props: AProps) { return <View />; }\n"}},
        "a type-alias import cycle");
    check_eq(cycle, "TESSERA_IR_TYPE_CYCLE", "alias import cycle code");

    const std::string missing_module = expect_error(
        {{"ui/Card.tsx", "import type { CardProps } from \"./missing\";\n"
                         "export function Card(props: CardProps) { return <View />; }\n"}},
        "an unresolvable module");
    check_eq(missing_module, "TESSERA_IR_UNRESOLVED", "missing module code");

    const std::set<std::string> codes{missing_export, non_type, cycle, missing_module};
    check(codes.size() == 4, "each failure mode has its own TESSERA_* code");
}

// A props alias reached through a package root uses the ordinary import graph (ADR-014, issue #70).
void test_package_root_alias_resolves() {
    Project project;
    project.add(parse_file("ui/Card.tsx", "import type { CardProps } from \"tessera-ui/types\";\n" +
                                              std::string(kCardComponent)));
    project.add(parse_file("packages/tessera-ui/types.tsx",
                           "export type CardProps = " + std::string(kPropsBody)));
    const std::array<lowering::PackageRoot, 1> packages{
        {{.name = "tessera-ui", .root = "packages/tessera-ui"}}};
    const std::vector<lowering::ModuleSource> sources = project.sources();
    const auto result = lowering::lower_project(sources, "demo", packages);
    check(result.has_value(), "a package-root props alias resolves: " + first_message(result));
    if (!result) {
        return;
    }
    const ir::Component* card = find_component(*result, "Card");
    check(card != nullptr, "the package-importing Card lowers");
    if (card != nullptr) {
        check_eq(props_text(*card), std::string(kExpectedProps),
                 "a package-root alias lowers the same records");
    }
}

// The codegen consequence: the props struct is a pure function of the IR, so an imported alias
// generates byte-identical C++ to the inline alias.
void test_generated_cpp_is_byte_identical() {
    auto inline_result =
        lower_files({{"ui/Card.tsx", "type CardProps = " + std::string(kPropsBody) +
                                         std::string(kCardComponent)}});
    auto imported_result =
        lower_files({{"ui/types.tsx", "export type CardProps = " + std::string(kPropsBody)},
                     {"ui/Card.tsx", "import type { CardProps } from \"./types\";\n" +
                                         std::string(kCardComponent)}});
    check(inline_result.has_value() && imported_result.has_value(), "both codegen fixtures lower");
    if (!inline_result || !imported_result) {
        return;
    }
    const auto into_module = [](lowering::LoweredModule&& lowered) {
        ir::IrModule module;
        module.name = std::move(lowered.name);
        module.components = std::move(lowered.components);
        module.bindings = std::move(lowered.bindings);
        module.stylesheets = std::move(lowered.stylesheets);
        return module;
    };
    auto inline_lowered = std::move(*inline_result);
    auto imported_lowered = std::move(*imported_result);
    const auto inline_cpp = codegen::generate(into_module(std::move(inline_lowered)));
    const auto imported_cpp = codegen::generate(into_module(std::move(imported_lowered)));
    check(inline_cpp.has_value() && imported_cpp.has_value(),
          "both fixtures generate C++: " +
              (imported_cpp ? std::string() : imported_cpp.error().front()));
    if (!inline_cpp || !imported_cpp) {
        return;
    }
    check(inline_cpp->size() == imported_cpp->size(), "the same number of generated files");
    const std::size_t count = std::min(inline_cpp->size(), imported_cpp->size());
    bool props_struct_seen = false;
    for (std::size_t i = 0; i < count; ++i) {
        check_eq((*imported_cpp)[i].path, (*inline_cpp)[i].path, "the same generated path");
        check_eq((*imported_cpp)[i].text, (*inline_cpp)[i].text,
                 "generated C++ for the imported alias is byte-identical");
        if ((*imported_cpp)[i].path == "Card.hpp") {
            props_struct_seen = (*imported_cpp)[i].text.find("prop_title_") != std::string::npos &&
                                (*imported_cpp)[i].text.find("prop_count_") != std::string::npos;
        }
    }
    check(props_struct_seen, "the generated props struct carries the declared fields");
}

} // namespace

int main() {
    test_inline_and_imported_aliases_are_identical();
    test_import_forms_resolve();
    test_optional_slot_and_default_through_the_import();
    test_package_root_alias_resolves();
    test_alias_resolution_errors_are_distinct();
    test_generated_cpp_is_byte_identical();
    return jsx_test::finish("lowering props alias tests");
}
