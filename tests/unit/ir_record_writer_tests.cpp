// SPDX-License-Identifier: MIT

// IR record types and record values, writer half (docs/specs/ir-v1.md sections 2.2, 4.3, 9.1, 11
// and 12; ADR-051): the `record` expression and the optional `types` array, and the proof that a
// module with no record is byte-identical to the IR before this slice.

#include "ir_record_util.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ir_record_test;

// The `record` expression writer (section 4.3).
void test_record_expression_writer() {
    check_eq(flat(expr_text(size_record(1, 2).get())),
             "{\"fields\": [{\"name\": \"w\",\"value\": {\"kind\": \"literal\",\"value\": 1}},"
             "{\"name\": \"h\",\"value\": {\"kind\": \"literal\",\"value\": 2}}],"
             "\"kind\": \"record\",\"type\": \"Size\"}",
             "a record value writes `type` and its fields in member order");
    check_eq(flat(expr_text(size_record({}).get())),
             "{\"fields\": [],\"kind\": \"record\",\"type\": \"Size\"}",
             "a record value with no field writes an empty `fields` array");
    std::vector<ir::RecordField> nested_fields;
    nested_fields.push_back({"padding", size_record(4, 8)});
    check_eq(flat(expr_text(size_record(std::move(nested_fields)).get())),
             "{\"fields\": [{\"name\": \"padding\",\"value\": {\"fields\": "
             "[{\"name\": \"w\",\"value\": {\"kind\": \"literal\",\"value\": 4}},"
             "{\"name\": \"h\",\"value\": {\"kind\": \"literal\",\"value\": 8}}],"
             "\"kind\": \"record\",\"type\": \"Size\"}}],\"kind\": \"record\",\"type\": \"Size\"}",
             "a nested record field keeps the object form");
}

// The optional `types` array (section 2.2).
void test_types_array_writer() {
    const std::string text = ir::write_ir_json(make_record_module());
    const std::string one_line = flat(text);
    const auto has = [&](std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    const auto has_flat = [&](std::string_view needle) {
        return one_line.find(needle) != std::string::npos;
    };
    check(has("\"types\": ["), "a module with records writes the `types` array");
    check(text.find("\"id\": \"type::Size\"") < text.find("\"id\": \"type::Style\""),
          "the type table is written in module order (sorted by name)");
    check(has("\"members\": ["), "a type writes its members");
    // A required member writes only its name and type; an optional member adds `optional: true`.
    check(has_flat("\"name\": \"w\",\"type\": \"int\""), "required member name and type");
    check(has("\"optional\": true"), "an optional member writes `optional: true`");
    check(has_flat("\"name\": \"sizes\",\"type\": \"list<int>\""),
          "a member keeps its declared IR type, wrapper included");
    // An optional member writes the flag between the name and the type (keys are in byte order).
    check(has_flat("\"name\": \"padding\",\"optional\": true,\"type\": \"Size\""),
          "an optional member keeps its declared type");
    check(has("\"kind\": \"record\""), "the record expression kind is written");
    // A field value is always an expression object: the writer's bare-literal rule applies to a
    // whole expression in a value position (a prop default), never to a field value (section 9.1).
    check(has_flat("\"value\": {\"kind\": \"literal\",\"value\": 10}"),
          "a literal field value is an expression object, never bare");
    // The writer is a pure function of the IR, so two writes agree and a write does not mutate.
    check_eq(ir::write_ir_json(make_record_module()), text,
             "the record module writes the same bytes");
}

// Section 12 (ADR-051): the additions are optional, so a module with no record keeps exactly the
// bytes the writer produced before `types` and `record` existed. `counter.json` and `rich.json` are
// the pre-existing goldens (`ir_json` pins them and this slice does not re-baseline them); this
// asserts the omission rule directly on a fresh module.
void test_no_record_module_is_byte_identical() {
    const std::string text = ir::write_ir_json(make_no_record_module());
    check(text.find("\"types\"") == std::string::npos, "a module with no records omits `types`");
    check(text.find("\"record\"") == std::string::npos, "a module with no records has no record");
    check(text.find("\"irVersion\": 1") != std::string::npos, "irVersion stays 1");
    // The three always-written lists are still there, so the omission is the only difference.
    check(text.find("\"components\": [") != std::string::npos &&
              text.find("\"bindings\": []") != std::string::npos &&
              text.find("\"stylesheets\": []") != std::string::npos,
          "the always-written lists are unchanged");
    // The module has the pre-`types` top-level shape: no record field at any level.
    check(text.find("\"members\"") == std::string::npos, "no member list either");
}

} // namespace

int main() {
    test_record_expression_writer();
    test_types_array_writer();
    test_no_record_module_is_byte_identical();
    return ir_test::finish("ir record writer tests");
}
