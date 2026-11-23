// SPDX-License-Identifier: MIT

// Anonymous record types in a props value position (issue #804, ADR-066 item 3): an inline
// `{ id: string; label: string }[]` element becomes a real `types` entry under a deterministic
// synthesized name, and a `.filter(...)`/`.slice(...)` chain lowers as a list loop source with the
// predicate's item name visible to it.

#include "lowering_module_test_util.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace lowering_module_test;
namespace ir = tessera::compiler::ir;
// The corpus fixture's shape: an anonymous record in an array prop, filtered, then mapped.
constexpr std::string_view kFilterMap =
    "export function A({ rows }: { rows: { id: string; label: string; on: boolean }[] }) {\n"
    "  return (\n"
    "    <View className=\"rows\">\n"
    "      {rows.filter((row) => row.on).map((row) => (\n"
    "        <Text key={row.id}>{row.label}</Text>\n"
    "      ))}\n"
    "    </View>\n"
    "  );\n"
    "}\n";

constexpr std::string_view kSliceMap =
    "export function RecentRows({ rows }: { rows: { id: string; label: string }[] }) {\n"
    "  return (\n"
    "    <View className=\"recent\">\n"
    "      {rows.slice(0, 5).map((row) => (\n"
    "        <Text key={row.id}>{row.label}</Text>\n"
    "      ))}\n"
    "    </View>\n"
    "  );\n"
    "}\n";

void test_anonymous_prop_record_is_collected() {
    // The props type is inline, so nothing but the synthesis can name it. The synthesized record
    // lands in `types` like any declared record, and the prop type names it.
    Project project;
    project.add(parse_file("ui/a.tsx", std::string(kFilterMap)));
    const lowering::ModuleLoweringResult result = project.lower("filter_map");
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    bool found_prop = false;
    bool found_record = false;
    for (const ir::Component& component : result->components) {
        for (const ir::Prop& prop : component.props) {
            if (prop.name == "rows" && prop.type.rfind("list<", 0) == 0) {
                found_prop = true;
                // The element names a record in the table, not a primitive: that is the whole
                // point of the synthesis.
                const std::string element = prop.type.substr(5, prop.type.size() - 6);
                for (const ir::RecordType& record : result->types) {
                    if (record.name == element) {
                        found_record = true;
                        check(record.members.size() == 3,
                              "the synthesized record has the three declared members");
                        check(record.members[0].name == "id" && record.members[0].type == "string",
                              "the first member is id:string");
                        check(record.members[2].name == "on" && record.members[2].type == "bool",
                              "the third member is on:boolean -> bool");
                    }
                }
            }
        }
    }
    check(found_prop, "the rows prop lowers to a list type");
    check(found_record, "the list element names a collected record");
}

void test_slice_map_lowers() {
    Project project;
    project.add(parse_file("ui/slice-map.tsx", std::string(kSliceMap)));
    const lowering::ModuleLoweringResult result = project.lower("slice_map");
    check(result.has_value(), first_message(result));
}

void test_declared_alias_wins_over_synthesis() {
    // A `type Row = ...` alias for the same shape keeps its declared name: the synthesized name is
    // only for types nothing names.
    constexpr std::string_view text =
        "type Row = { id: string; label: string };\n"
        "export function RecentRows({ rows }: { rows: Row[] }) {\n"
        "  return (\n"
        "    <View>{rows.map((row) => <Text key={row.id}>{row.label}</Text>)}</View>\n"
        "  );\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/slice-map.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("slice_map");
    check(result.has_value(), first_message(result));
    if (!result) {
        return;
    }
    check(result->types.size() == 1 && result->types.front().name == "Row",
          "the declared alias is the only record");
}

void test_unknown_member_type_lowering_is_permissive() {
    // A member whose type is an identifier lowers permissively: the type-table check that would
    // reject an unknown name is the IR validator's job, not the collector's. The synthesized
    // record is collected with the member, and validation reports the unknown type.
    constexpr std::string_view text =
        "export function Bad({ rows }: { rows: { id: string; kids: SomeUnknown[] }[] }) {\n"
        "  return (\n"
        "    <View>{rows.map((row) => <Text key={row.id} />)}</View>\n"
        "  );\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/bad.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("bad");
    check(result.has_value(), first_message(result));
}

void test_loop_source_chain_lowers_and_key_sees_item() {
    // The loop source is a `.filter(...)` chain; the template body and the key read the loop item,
    // which must resolve inside the template (the loop binds its item; the filter predicate binds
    // its own, in the separate scope the predicate lowering pushes).
    constexpr std::string_view text =
        "export function Rows({ rows }: { rows: string[] }) {\n"
        "  return (\n"
        "    <View>{rows.filter((r) => true).map((row) => <Text key={row}>{row}</Text>)}</View>\n"
        "  );\n"
        "}\n";
    Project project;
    project.add(parse_file("ui/rows.tsx", std::string(text)));
    const lowering::ModuleLoweringResult result = project.lower("rows");
    check(result.has_value(), first_message(result));
}

} // namespace

int main() {
    test_anonymous_prop_record_is_collected();
    test_slice_map_lowers();
    test_declared_alias_wins_over_synthesis();
    test_unknown_member_type_lowering_is_permissive();
    test_loop_source_chain_lowers_and_key_sees_item();
    if (failures == 0) {
        std::cout << "lowering anonymous record tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
