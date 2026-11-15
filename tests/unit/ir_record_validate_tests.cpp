// SPDX-License-Identifier: MIT

// IR record types and record values, validator half (docs/specs/ir-v1.md sections 2.2, 4.3, 9 and
// 11; ADR-051): the type-table rules, cycles, the field rules, the positions a record value is
// allowed in, dotted refs, the unused-record warning, and the predeclared payload records. The
// writer half is ir_record_writer_tests.cpp.

#include "ir_record_util.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_record_test;

// --- type table rules ------------------------------------------------------------------------

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

void test_record_value_positions() {
    expect_record("a valid record value in both positions", [](auto&) {}, "");
    // Section 4.3: a `useState` initial of a record type must be a record value of exactly that
    // record, and a prop default of a non-record type never takes one.
    // A `Style` state declared with a `Size` value: the value is well formed but is not the
    // declared record, which is one mismatch.
    expect_record(
        "a record state initial of the wrong record",
        [](auto& m) {
            m.components[0].state[0].type = "Style";
            m.components[0].state[0].initial = size_record(1, 2);
        },
        std::string(kRecordValue));
    expect_record(
        "a scalar initial in a record position",
        [](auto& m) { m.components[0].state[0].initial = ir::make_literal(std::int64_t{1}); },
        std::string(kRecordValue));
    expect_record(
        "a record value where the declared type is not a record",
        [](auto& m) { m.components[0].props[2].default_value = size_record(1, 2); },
        std::string(kRecordValue));
    // A record value is not a binary operand, a condition or a template part: only its members take
    // part there, so a record in those positions is the section 4.3 error.
    expect_record(
        "a record value in a binary expression",
        [](auto& m) {
            m.components[0].root.children[0].text =
                ir::make_binary("+", size_record(1, 2), ir::make_literal(std::int64_t{1}));
        },
        std::string(kRecordValue));
    expect_record(
        "a record value as a condition",
        [](auto& m) { m.components[0].root.children[0].condition = size_record(1, 2); },
        std::string(kRecordValue));
    // A record value nested in a template is also not an allowed position. The template is rebuilt
    // inside the lambda because a `TemplatePart` holds a `unique_ptr`.
    expect_record(
        "a record value in a template",
        [](auto& m) {
            std::vector<ir::TemplatePart> parts;
            parts.push_back(ir::expr_part(size_record(1, 2)));
            m.components[0].root.children[0].text = ir::make_template(std::move(parts));
        },
        std::string(kRecordValue));
    // A record value as a field of another record value is the third allowed position, but the
    // field's declared type must be the nested record's.
    expect_record(
        "a record value as a field of another record value",
        [](auto& m) { m.components[0].props[1].default_value = size_record(1, 2); },
        std::string(kRecordValue));
    // An undeclared record name is the unknown-type rule, and the position then reports the
    // record-value mismatch.
    expect_record(
        "a record value type that is not in the table",
        [](auto& m) { m.components[0].state[0].type = "Missing"; },
        std::string(kUnknownType) + kRecordValue);
    // A payload record is a real type but has no aggregate value form.
    expect_record(
        "a record value whose type is a payload record",
        [](auto& m) {
            m.components[0].state[0].type = "TextChangeEvent";
            m.components[0].state[0].initial = ir::make_record("TextChangeEvent", {});
        },
        std::string(kUnsupported));
}

void test_record_field_rules() {
    // A missing required member: one report for the missing member.
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", ir::make_literal(std::int64_t{1})});
        expect_record(
            "a missing required field",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kRecordValue));
    }
    // A duplicate field: the duplicate and the now-missing `h`.
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", ir::make_literal(std::int64_t{1})});
        fields.push_back({"w", ir::make_literal(std::int64_t{2})});
        expect_record(
            "a duplicate field",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kRecordValue) + kRecordValue);
    }
    // An unknown field, plus the required `h` the value still lacks.
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", ir::make_literal(std::int64_t{1})});
        fields.push_back({"h", ir::make_literal(std::int64_t{2})});
        fields.push_back({"z", ir::make_literal(std::int64_t{3})});
        expect_record(
            "an unknown field",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kRecordValue));
    }
    // Fields out of member order (section 2.2: declaration order is the aggregate order).
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"h", ir::make_literal(std::int64_t{2})});
        fields.push_back({"w", ir::make_literal(std::int64_t{1})});
        expect_record(
            "fields out of member order",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            "TESSERA_IR_ORDER;");
    }
    // A field value of the wrong type (section 4.1: no implicit float-to-int).
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", ir::make_literal(std::string("wide"))});
        fields.push_back({"h", ir::make_literal(std::int64_t{2})});
        expect_record(
            "a field value of the wrong type",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kRecordValue));
    }
    // An `int` member never accepts a float literal, though an `int` literal does convert to a
    // `float` member exactly (section 4.1).
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", ir::make_literal(1.5)});
        fields.push_back({"h", ir::make_literal(std::int64_t{2})});
        expect_record(
            "a float literal in an int field",
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kRecordValue));
    }
    // An optional member may be omitted; a required one may not.
    {
        std::vector<ir::RecordField> fields;
        fields.push_back({"color", ir::make_literal(std::string("#fff"))});
        fields.push_back({"sizes", ir::make_array({})});
        expect_record(
            "an omitted optional field",
            [&](auto& m) {
                m.components[0].props[1].default_value =
                    ir::make_record("Style", std::move(fields));
            },
            "");
    }
    {
        // Only the optional `padding` is written, so the two required members are missing.
        std::vector<ir::RecordField> fields;
        fields.push_back({"padding", size_record(4, 8)});
        expect_record(
            "two omitted required fields",
            [&](auto& m) {
                m.components[0].props[1].default_value =
                    ir::make_record("Style", std::move(fields));
            },
            std::string(kRecordValue) + kRecordValue);
    }
    {
        // One required member omitted: `color` is written, `sizes` is not.
        std::vector<ir::RecordField> fields;
        fields.push_back({"color", ir::make_literal(std::string("#fff"))});
        expect_record(
            "an omitted required field",
            [&](auto& m) {
                m.components[0].props[1].default_value =
                    ir::make_record("Style", std::move(fields));
            },
            std::string(kRecordValue));
    }
    // A nested record field must be the declared member record: `padding` is a `Size`, so a `Style`
    // value there is the mismatch plus the fields the outer value then lacks.
    {
        std::vector<ir::RecordField> nested;
        nested.push_back({"color", ir::make_literal(std::string("#fff"))});
        nested.push_back({"sizes", ir::make_array({})});
        std::vector<ir::RecordField> fields;
        fields.push_back({"color", ir::make_literal(std::string("#fff"))});
        fields.push_back({"sizes", ir::make_array({})});
        fields.push_back({"padding", ir::make_record("Style", std::move(nested))});
        expect_record(
            "a nested record field of the wrong record",
            [&](auto& m) {
                m.components[0].props[1].default_value =
                    ir::make_record("Style", std::move(fields));
            },
            std::string(kRecordValue));
    }
}

void test_record_dynamic_field_values() {
    // Section 4.3: the first slice accepts a literal, a nested record of literals or an array of
    // literals. A `ref` or any other dynamic expression is `TESSERA_IR_UNSUPPORTED`.
    const auto check_dynamic = [](const std::string& what, ir::ExprPtr value) {
        std::vector<ir::RecordField> fields;
        fields.push_back({"w", std::move(value)});
        fields.push_back({"h", ir::make_literal(std::int64_t{2})});
        expect_record(
            what,
            [&](auto& m) { m.components[0].state[0].initial = size_record(std::move(fields)); },
            std::string(kUnsupported));
    };
    check_dynamic("a ref field value", ir::make_ref("state::size"));
    check_dynamic("a binary field value", ir::make_binary("+", ir::make_literal(std::int64_t{1}),
                                                          ir::make_literal(std::int64_t{2})));
    {
        // An argument list is built explicitly: a braced list of `unique_ptr` would need to copy.
        std::vector<ir::ExprPtr> args;
        args.push_back(ir::make_literal(std::int64_t{1}));
        check_dynamic("a call field value", ir::make_call("abs", std::move(args)));
    }
    // An array in a scalar member: the value is not a literal of the member type.
    {
        std::vector<ir::ExprPtr> items;
        items.push_back(ir::make_literal(std::int64_t{1}));
        check_dynamic("an array field value in a scalar member", ir::make_array(std::move(items)));
    }
    // A `list<record>` member is declarable, but its value form is not in this slice: an array of
    // records is not an array of literals.
    {
        std::vector<ir::ExprPtr> items;
        items.push_back(size_record(1, 2));
        std::vector<ir::RecordField> fields;
        fields.push_back({"color", ir::make_literal(std::string("#fff"))});
        fields.push_back({"sizes", ir::make_array(std::move(items))});
        expect_record(
            "a list field with a record element",
            [&](auto& m) {
                m.components[0].props[1].default_value =
                    ir::make_record("Style", std::move(fields));
            },
            std::string(kUnsupported));
    }
}

void test_dotted_reference_members() {
    expect_record("a dotted ref to a real member", [](auto&) {}, "");
    expect_record(
        "a dotted ref to a member the record lacks",
        [](auto& m) { m.components[0].root.children[0].text = ir::make_ref("prop::size.z"); },
        std::string(kUnresolved));
    expect_record(
        "a dotted ref into an optional member is still checked",
        [](auto& m) {
            m.components[0].root.children[0].text = ir::make_ref("prop::style.padding.w");
        },
        "");
    expect_record(
        "a dotted ref to a missing nested member",
        [](auto& m) {
            m.components[0].root.children[0].text = ir::make_ref("prop::style.padding.q");
        },
        std::string(kUnresolved));
    expect_record(
        "a dotted ref into a state record",
        [](auto& m) { m.components[0].root.children[0].text = ir::make_ref("state::size.h"); }, "");
    expect_record(
        "a dotted ref to a missing state record member",
        [](auto& m) { m.components[0].root.children[0].text = ir::make_ref("state::size.q"); },
        std::string(kUnresolved));
    // A dotted ref whose base does not resolve is the ordinary unresolved rule.
    expect_record(
        "a dotted ref with an unknown base",
        [](auto& m) { m.components[0].root.children[0].text = ir::make_ref("state::gone.w"); },
        std::string(kUnresolved));
    // A dotted ref into a payload record is not member-checked in this slice: the element schema
    // owns that member table (section 9.1, "deliberately not covered").
    expect_record(
        "a dotted ref into a payload record",
        [](auto& m) {
            m.components[0].props.push_back({"change", "TextChangeEvent", nullptr});
            m.components[0].root.children[0].text = ir::make_ref("prop::change.anything");
        },
        "");
}

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
    test_record_value_positions();
    test_record_field_rules();
    test_record_dynamic_field_values();
    test_dotted_reference_members();
    test_unused_type_warning();
    test_predeclared_records();
    return finish("ir record validator tests");
}
