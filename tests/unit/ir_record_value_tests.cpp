// SPDX-License-Identifier: MIT

// IR record values, validator half (docs/specs/ir-v1.md sections 4.3 and 9; ADR-051): the three
// positions a record value is allowed in, the field set and field order, member-typed field values,
// the first slice's literal/nested-record/array forms, and dotted refs into a declared record. The
// type table rules are in ir_record_table_tests.cpp; the writer is ir_record_writer_tests.cpp.

#include "ir_record_util.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ir_record_test;
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

} // namespace

int main() {
    test_record_value_positions();
    test_record_field_rules();
    test_record_dynamic_field_values();
    test_dotted_reference_members();
    return finish("ir record value validator tests");
}
