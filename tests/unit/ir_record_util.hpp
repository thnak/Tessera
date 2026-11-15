// SPDX-License-Identifier: MIT

#pragma once

// Shared helpers of the two IR record test translation units: the expression/writer helpers and the
// `expect_*` wrappers. `ir_record_tests.cpp` covers the writer, `ir_record_validate_tests.cpp` the
// validator; both keep the established IR test style (`ir_test_util.hpp`).

#include "ir_samples.hpp"
#include "ir_test_util.hpp"
#include "ir_validate_util.hpp"
#include "tessera/compiler/ir/ir_json.hpp"
#include "tessera/compiler/ir/types.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ir_record_test {

namespace ir = tessera::compiler::ir;

// The IR test style is a free `check`/`check_eq` (`ir_test_util.hpp`) and the fixture factories
// (`ir_samples.hpp`); re-export them so a test TU needs one `using namespace` line.
using ir_test::check;
using ir_test::check_eq;
using ir_test::codes_of;
using ir_test::finish;
using ir_test::kDuplicate;
using ir_test::kMalformed;
using ir_test::kRecordValue;
using ir_test::kUnknownType;
using ir_test::kUnresolved;
using ir_test::kUnsupported;
using ir_test::make_no_record_module;
using ir_test::make_record_module;
using ir_test::span;

inline std::string expr_text(const ir::Expr* expr) {
    return ir::to_text(ir::expr_to_json(expr));
}

// Collapses the writer's pretty-printed JSON to one line for a compact expected string.
inline std::string flat(std::string text) {
    std::string out;
    bool skipping = false;
    for (const char c : text) {
        if (c == '\n') {
            skipping = true;
        } else if (skipping && c == ' ') {
            continue;
        } else {
            skipping = false;
            out += c;
        }
    }
    return out;
}

// A `Size { w, h }` record value of two int literals.
inline ir::ExprPtr size_record(std::int64_t w, std::int64_t h) {
    std::vector<ir::RecordField> fields;
    fields.push_back({"w", ir::make_literal(w)});
    fields.push_back({"h", ir::make_literal(h)});
    return ir::make_record("Size", std::move(fields));
}

inline ir::ExprPtr size_record(std::vector<ir::RecordField> fields) {
    return ir::make_record("Size", std::move(fields));
}

// The `Size` and `Style` table entries of the table-only fixture.
inline ir::RecordType table_size() {
    return {"type::Size",
            "Size",
            {{"w", "int", false}, {"h", "int", false}},
            ir_test::span(0, 40, 1, 1, 1, 41)};
}

inline ir::RecordType table_style() {
    return {"type::Style",
            "Style",
            {{"color", "string", false}, {"padding", "Size", true}},
            ir_test::span(42, 60, 2, 1, 4, 2)};
}

// A fresh full record fixture with `change` applied, and the diagnostic codes in order.
inline void expect_record(const std::string& what, const std::function<void(ir::IrModule&)>& change,
                          const std::string& expected) {
    ir::IrModule module = ir_test::make_record_module();
    change(module);
    ir_test::check_eq(ir_test::codes_of(module), expected, what);
}

// A module with only a type table and no component, so a table rule is asserted without the
// cascade a broken member type causes in the record values of the full fixture. `codes_of` then
// ends with one `TESSERA_IR_UNUSED_TYPE` per declared record (section 9, a warning).
inline void expect_table(const std::string& what, const std::function<void(ir::IrModule&)>& change,
                         const std::string& expected) {
    ir::IrModule module;
    change(module);
    ir_test::check_eq(ir_test::codes_of(module), expected, what);
}

} // namespace ir_record_test
