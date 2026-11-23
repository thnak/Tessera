// SPDX-License-Identifier: MIT

// ADR-051 slice 4 (issue #158): codegen emits a model with a record type table as one per-module
// `<module>_types.hpp` of aggregate structs, a record default/initial as designated aggregate
// initialization, and one include edge in each generated component header that names a record. The
// lowering half of the same fixture (the `ir::Record` shape and the table) is asserted in
// `lowering_props_tests.cpp`; this file lowers and generates so the emitted text is proven end to
// end. A module without records must emit no types header and no extra include, which the committed
// `.g.hpp` goldens already prove and `test_record_free_modules_emit_no_types_header` pins.

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

ir::IrModule into_module(lowering::LoweredModule&& lowered) {
    ir::IrModule module;
    module.name = std::move(lowered.name);
    module.components = std::move(lowered.components);
    module.bindings = std::move(lowered.bindings);
    module.stylesheets = std::move(lowered.stylesheets);
    module.types = std::move(lowered.types);
    return module;
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

void test_package_root_alias_resolves() {
    Project project;
    project.add(
        parse_file("packages/ui/types.tsx", "export type CardProps = " + std::string(kPropsBody)));
    project.add(parse_file("ui/Card.tsx", "import type { CardProps } from \"ui/types\";\n" +
                                              std::string(kCardComponent)));
    const std::vector<lowering::ModuleSource> sources = project.sources();
    const std::array<lowering::PackageRoot, 1> packages{lowering::PackageRoot{"ui", "packages/ui"}};
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
    const auto inline_cpp = codegen::generate(into_module(std::move(*inline_result)));
    const auto imported_cpp = codegen::generate(into_module(std::move(*imported_result)));
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
        if ((*imported_cpp)[i].path == "Card.g.hpp") {
            props_struct_seen = (*imported_cpp)[i].text.find("prop_title_") != std::string::npos &&
                                (*imported_cpp)[i].text.find("prop_count_") != std::string::npos;
        }
    }
    check(props_struct_seen, "the generated props struct carries the declared fields");
}

// ADR-051 slice 4 (issue #158): the record struct, the designated initializer and the include edge.
// The lowering assertions for the same fixture live in `lowering_props_tests.cpp`.
constexpr std::string_view kRecordSource = "type Spacing = {\n"
                                           "  top: int;\n"
                                           "  bottom: int;\n"
                                           "  label?: string;\n"
                                           "};\n"
                                           "type BoxProps = {\n"
                                           "  padding: Spacing;\n"
                                           "  title: string;\n"
                                           "  children: Children;\n"
                                           "};\n"
                                           "export function Box({ padding = { top: 4, bottom: 4 }, "
                                           "title, children }: BoxProps) {\n"
                                           "  const [margin, setMargin] = "
                                           "useState<Spacing>({ label: \"m\", bottom: 2, top: 1 "
                                           "});\n"
                                           "  return <View><Text>{title}</Text>{children}</View>;\n"
                                           "}\n";

const codegen::GeneratedFile* find_file(const std::vector<codegen::GeneratedFile>& files,
                                        std::string_view path) {
    for (const codegen::GeneratedFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

void test_record_type_and_value_are_emitted() {
    auto result = lower_files({{"ui/Box.tsx", std::string(kRecordSource)}});
    check(result.has_value(), "the record fixture lowers: " + first_message(result));
    if (!result) {
        return;
    }
    const auto generated = codegen::generate(into_module(std::move(*result)));
    check(generated.has_value(), "the record module generates C++: " +
                                     (generated ? std::string() : generated.error().front()));
    if (!generated) {
        return;
    }
    const codegen::GeneratedFile* types = find_file(*generated, "demo_types.hpp");
    check(types != nullptr, "a module with a record emits demo_types.hpp");
    if (types != nullptr) {
        check(types->text.find("struct Spacing {") != std::string::npos,
              "the aggregate struct is emitted");
        check(types->text.find("int top = {};") != std::string::npos,
              "the member keeps its C++ type");
        check(types->text.find("std::optional<std::string> label = {};") != std::string::npos,
              "an optional member becomes std::optional");
    }
    const codegen::GeneratedFile* header = find_file(*generated, "Box.g.hpp");
    check(header != nullptr, "the component header is generated");
    if (header == nullptr) {
        return;
    }
    check(header->text.find("#include \"demo_types.hpp\"") != std::string::npos,
          "the component header includes the module types header");
    check(header->text.find("Spacing prop_padding_ = Spacing{.top = 4, .bottom = 4};") !=
              std::string::npos,
          "the prop default is designated aggregate initialization");
    check(header->text.find("Spacing state_margin_ = Spacing{.top = 1, .bottom = 2, .label = "
                            "\"m\"};") != std::string::npos,
          "the state initial is designated aggregate initialization");
    // ADR-051 implementation note (#834): a record now has generated equality and per-field dirty
    // bits. The generated base is one header-only file (ADR-040), so the definition lives in the
    // header too. The whole-record setter compares field by field and raises only the changed
    // field's bit before scheduling once.
    const std::size_t setter = header->text.find("::set_margin(Spacing value) {");
    check(setter != std::string::npos, "the record state gets a setter taking the aggregate");
    if (setter != std::string::npos) {
        const std::string body = header->text.substr(setter, 700);
        check(body.find("state_margin_.top == value.top") != std::string::npos &&
                  body.find("state_margin_.bottom == value.bottom") != std::string::npos,
              "the record setter compares field by field");
        check(body.find("note_dirty(kBit_state_margin_top_)") != std::string::npos,
              "a changed field raises its own leaf bit");
        check(body.find("if (tessera_changed)") != std::string::npos,
              "the record setter schedules only when a field changed");
    }
    if (types != nullptr) {
        check(types->text.find("bool operator==(const Spacing&) const = default;") !=
                  std::string::npos,
              "the record aggregates get a defaulted operator==");
        check(types->text.find("inline std::uint64_t hash_value(const Spacing& value)") !=
                  std::string::npos,
              "the record aggregates get a field-wise hash");
    }
}

// The converse, which is what keeps every committed golden byte-identical: a module without a
// record table emits no types header and no component gains an include.
void test_record_free_modules_emit_no_types_header() {
    auto result = lower_files({{"ui/Card.tsx", "type CardProps = " + std::string(kPropsBody) +
                                                   std::string(kCardComponent)}});
    check(result.has_value(), "the record-free fixture lowers: " + first_message(result));
    if (!result) {
        return;
    }
    check(result->types.empty(),
          "a module with no `type`-declared value record has an empty table");
    const auto generated = codegen::generate(into_module(std::move(*result)));
    check(generated.has_value(), "the record-free module generates C++");
    if (!generated) {
        return;
    }
    for (const codegen::GeneratedFile& file : *generated) {
        check(file.path.find("_types.hpp") == std::string::npos,
              "no module types header is emitted: " + file.path);
        check(file.text.find("_types.hpp") == std::string::npos,
              "no generated file mentions a types header: " + file.path);
    }
}

} // namespace

int main() {
    test_inline_and_imported_aliases_are_identical();
    test_import_forms_resolve();
    test_optional_slot_and_default_through_the_import();
    test_package_root_alias_resolves();
    test_alias_resolution_errors_are_distinct();
    test_record_type_and_value_are_emitted();
    test_record_free_modules_emit_no_types_header();
    test_generated_cpp_is_byte_identical();
    return jsx_test::finish("lowering props alias tests");
}
