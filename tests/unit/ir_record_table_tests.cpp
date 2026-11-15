// SPDX-License-Identifier: MIT

// IR record type table, validator half (docs/specs/ir-v1.md sections 2.2, 9 and 11; ADR-051): the
// table rules, member types, sort order, cycles, the unused-record warning and the predeclared
// payload records. The record *value* rules are in ir_record_value_tests.cpp; the writer is
// ir_record_writer_tests.cpp.

#include "ir_record_util.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_record_test;
void test_type_table_rules() {
    // A clean table still warns about each unused record (section 9): that is the only finding.
    expect_table(
        "a valid table is clean apart from its unused warnings",
        [](auto& m) {
            m.types.push_back(table_size());
            m.types.push_back(table_style());
        },
        "TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "types must be sorted by name",
        [](auto& m) {
            m.types.push_back(table_style());
            m.types.push_back(table_size());
        },
        "TESSERA_IR_ORDER;TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a duplicate record name",
        [](auto& m) {
            m.types.push_back(table_size());
            m.types.push_back(table_size());
        },
        "TESSERA_IR_ORDER;TESSERA_IR_DUPLICATE_ID;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a record id that is not type::<name>",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.id = "type::Wrong";
            m.types.push_back(type);
        },
        "TESSERA_IR_MALFORMED;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a duplicate member name",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[1].name = "w";
            m.types.push_back(type);
        },
        "TESSERA_IR_DUPLICATE_ID;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a member with no name",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].name.clear();
            m.types.push_back(type);
        },
        "TESSERA_IR_MALFORMED;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a record with a bad span",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.source.file.clear();
            m.types.push_back(type);
        },
        "TESSERA_IR_SPAN;TESSERA_IR_UNUSED_TYPE;");
    // Section 2.2: a member is a value, so a `callback` or the `children` slot has no record form.
    expect_table(
        "a member of a callback type",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "callback";
            m.types.push_back(type);
        },
        "TESSERA_IR_UNSUPPORTED;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a member of the children slot type",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "children";
            m.types.push_back(type);
        },
        "TESSERA_IR_UNSUPPORTED;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a member of an undeclared record type",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "Missing";
            m.types.push_back(type);
        },
        "TESSERA_IR_UNKNOWN_TYPE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a member of a malformed type",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "int[]";
            m.types.push_back(type);
        },
        "TESSERA_IR_UNSUPPORTED;TESSERA_IR_UNUSED_TYPE;");
    // A member type may be a payload record the framework predeclares: it is valid, and it is not a
    // project declaration, so it never needs an entry in `types`.
    expect_table(
        "a member of a payload record type",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "TextChangeEvent";
            m.types.push_back(type);
        },
        "TESSERA_IR_UNUSED_TYPE;");
    // A list-of-record member is declarable even though this slice has no value form for it.
    expect_table(
        "a list of records member is declarable",
        [](auto& m) {
            m.types.push_back(table_size());
            ir::RecordType type = table_style();
            type.members.push_back({"sizes", "list<Size>", false});
            m.types.push_back(type);
        },
        "TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
}

void test_type_cycles() {
    // A cycle rejects the module, but the unused warning still follows the declarations, so it
    // appears after the cycle for each record the cycle left unused.
    expect_table(
        "a direct cycle",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "Size";
            m.types.push_back(type);
        },
        "TESSERA_IR_TYPE_CYCLE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a cycle through a list",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "list<Size>";
            m.types.push_back(type);
        },
        "TESSERA_IR_TYPE_CYCLE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a cycle through an optional member",
        [](auto& m) {
            ir::RecordType type = table_size();
            type.members[0].type = "optional<Size>";
            m.types.push_back(type);
        },
        "TESSERA_IR_TYPE_CYCLE;TESSERA_IR_UNUSED_TYPE;");
    // An indirect cycle: `Style` holds itself, so both declared records are unused.
    expect_table(
        "an indirect cycle",
        [](auto& m) {
            m.types.push_back(table_size());
            ir::RecordType style = table_style();
            style.members[1].type = "Style";
            m.types.push_back(style);
        },
        "TESSERA_IR_TYPE_CYCLE;TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
    expect_table(
        "a three-record ring",
        [](auto& m) {
            m.types.push_back({"type::A", "A", {{"b", "B", false}}, span(0, 4, 1, 1, 1, 5)});
            m.types.push_back({"type::B", "B", {{"c", "C", false}}, span(5, 4, 2, 1, 2, 5)});
            m.types.push_back({"type::C", "C", {{"a", "A", false}}, span(10, 4, 3, 1, 3, 5)});
        },
        "TESSERA_IR_TYPE_CYCLE;TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;"
        "TESSERA_IR_UNUSED_TYPE;");
    // A diamond (`Left -> Size`, `Right -> Size`) is not a cycle, so the three unused warnings
    // appear and the module is accepted. The table is in the section 11 order (`Left`, `Right`,
    // `Size`), so no order finding is added.
    expect_table(
        "a diamond is not a cycle",
        [](auto& m) {
            m.types.push_back(
                {"type::Left", "Left", {{"size", "Size", false}}, span(50, 10, 3, 1, 3, 11)});
            m.types.push_back(
                {"type::Right", "Right", {{"size", "Size", false}}, span(62, 10, 4, 1, 4, 11)});
            m.types.push_back(table_size());
        },
        "TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
    // A table in any other than sorted order is the section 11 order rule, exactly once.
    expect_table(
        "an unsorted table is out of order",
        [](auto& m) {
            m.types.push_back(table_size());
            m.types.push_back(
                {"type::Left", "Left", {{"size", "Size", false}}, span(50, 10, 3, 1, 3, 11)});
        },
        "TESSERA_IR_ORDER;TESSERA_IR_UNUSED_TYPE;TESSERA_IR_UNUSED_TYPE;");
}

// --- record values ---------------------------------------------------------------------------

void test_unused_type_warning() {
    // Section 9: a declared record no component uses is a warning, not an error. The fixture's
    // table is `Size`, `Style`; `Unused` sorts after both, so appending keeps the order rule happy.
    ir::IrModule sorted = make_record_module();
    sorted.types.push_back(
        {"type::Unused", "Unused", {{"x", "int", false}}, span(140, 20, 8, 1, 8, 21)});
    check_eq(codes_of(sorted), "TESSERA_IR_UNUSED_TYPE;",
             "the unused record warns once and nothing else");
    // Inserting it before `Size` breaks the order rule as well, so both codes appear.
    ir::IrModule unsorted = make_record_module();
    unsorted.types.insert(
        unsorted.types.begin(),
        {"type::Unused", "Unused", {{"x", "int", false}}, span(140, 20, 8, 1, 8, 21)});
    check_eq(codes_of(unsorted), "TESSERA_IR_ORDER;TESSERA_IR_UNUSED_TYPE;",
             "an out-of-order unused record is reported out of order and unused");
    // A record named by a component is used even when only another record's member mentions it:
    // `Size` is used because the `Style` prop is and `Style` has a `Size` member. Drop the direct
    // `Size` uses (the prop and the state) but keep the `Style` prop and the ref it reads.
    ir::IrModule member_only = make_record_module();
    member_only.components[0].props.erase(member_only.components[0].props.begin());
    member_only.components[0].state.clear();
    member_only.components[0].root.children[0].text = ir::make_ref("prop::style.sizes");
    check_eq(codes_of(member_only), "",
             "a record used only as another used record's member counts as used");
    // A record reachable only from an unused record stays unused. `Orphan` sorts before `Size`, so
    // it goes at the front to keep the table order valid.
    ir::IrModule orphan = make_record_module();
    orphan.types.insert(
        orphan.types.begin(),
        {"type::Orphan", "Orphan", {{"size", "Size", false}}, span(140, 20, 8, 1, 8, 21)});
    check_eq(codes_of(orphan), "TESSERA_IR_UNUSED_TYPE;",
             "an unused record's member record is not thereby used");
    // Two declarations of an unused name: the duplicate id is reported once, and the dead entry
    // warns once (for the first declaration), not once per entry. `Unused` is appended twice in
    // sorted position (equal names fail the strict order rule, which is reported too).
    ir::IrModule duplicate = make_record_module();
    for (int i = 0; i < 2; ++i) {
        duplicate.types.push_back(
            {"type::Unused", "Unused", {{"x", "int", false}}, span(140, 20, 8, 1, 8, 21)});
    }
    check_eq(codes_of(duplicate),
             "TESSERA_IR_ORDER;TESSERA_IR_DUPLICATE_ID;TESSERA_IR_UNUSED_TYPE;",
             "a duplicate unused declaration warns once");
    // An empty module declares no record, so it has no unused warning.
    ir::IrModule empty;
    check_eq(codes_of(empty), "", "an empty module is clean");
}

// --- predeclared payload records -------------------------------------------------------------

void test_predeclared_records() {
    const std::span<const std::string_view> names = ir::predeclared_record_names();
    check(names.size() == 12, "the framework predeclares twelve payload records");
    check(ir::is_predeclared_record("TextChangeEvent"), "TextChangeEvent is predeclared");
    check(ir::is_predeclared_record("ClickEvent"), "ClickEvent is predeclared");
    check(!ir::is_predeclared_record("Size"), "a project record is not predeclared");
    check(!ir::is_predeclared_record("callback"), "`callback` is a keyword type, not a record");
    // They are never listed in `types`, so a payload record used as a prop type is not a
    // declaration and raises no unused warning.
    ir::IrModule module = make_record_module();
    module.components[0].props.push_back({"change", "TextChangeEvent", nullptr});
    check_eq(codes_of(module), "", "a payload record type raises no record declaration finding");
}

} // namespace

int main() {
    test_type_table_rules();
    test_type_cycles();
    test_unused_type_warning();
    test_predeclared_records();
    return finish("ir record table validator tests");
}
