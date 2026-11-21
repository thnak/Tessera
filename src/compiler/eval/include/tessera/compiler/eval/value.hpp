// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tessera::compiler::eval {

struct Value;
using List = std::vector<Value>;

struct Null {
    [[nodiscard]] bool operator==(const Null&) const = default;
};

// A record value (ADR-051 slice 5). Declared here and defined below `Value`, because a field holds
// a `Value` by value and `Value` holds a `Record` by value: the cycle is broken by making the
// variant's alternative a pointer-free forward declaration that is completed before any use.
//
// `type` is the declared record name (`Size`), which is what the IR carries and what a diagnostic
// names. `fields` are in the record type's member order, which is the order the IR validator
// enforced and the order the generated aggregate uses.
struct Record;

// A runtime value of the IR expression language (docs/specs/ir-v1.md section 4): null, bool, a
// signed 32-bit `int`, an IEEE-754 binary64 `float`, a string (UTF-8 bytes), a list, or a record
// (ADR-051). The numeric types stay distinct, as in generated C++ (`int` and `double`), because the
// language's rules depend on them (section 4.1).
struct Value {
    std::variant<Null, bool, std::int32_t, double, std::string, List, std::shared_ptr<Record>> data;

    [[nodiscard]] static Value null();
    [[nodiscard]] static Value of_bool(bool value);
    [[nodiscard]] static Value of_int(std::int32_t value);
    [[nodiscard]] static Value of_float(double value);
    [[nodiscard]] static Value of_string(std::string value);
    [[nodiscard]] static Value of_list(List value);
    [[nodiscard]] static Value of_record(Record value);

    [[nodiscard]] const bool* as_bool() const noexcept {
        return std::get_if<bool>(&data);
    }
    [[nodiscard]] const std::int32_t* as_int() const noexcept {
        return std::get_if<std::int32_t>(&data);
    }
    [[nodiscard]] const double* as_float() const noexcept {
        return std::get_if<double>(&data);
    }
    [[nodiscard]] const std::string* as_string() const noexcept {
        return std::get_if<std::string>(&data);
    }
    [[nodiscard]] const List* as_list() const noexcept {
        return std::get_if<List>(&data);
    }
    [[nodiscard]] const Record* as_record() const noexcept;
    [[nodiscard]] bool is_number() const noexcept {
        return as_int() != nullptr || as_float() != nullptr;
    }
    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<Null>(data);
    }
    // The number as a double (an int converts exactly); only meaningful when is_number().
    [[nodiscard]] double number() const noexcept;
};

// One field of a record value: the member name and its value, in the record type's member order.
// The name is kept because the evaluator has no type table: a dotted read (`state.count`) resolves
// by name, and `describe` prints the names.
struct RecordField {
    std::string name;
    Value value;
};

struct Record {
    std::string type;
    std::vector<RecordField> fields;

    [[nodiscard]] const Value* find(std::string_view name) const noexcept {
        for (const RecordField& field : fields) {
            if (field.name == name) {
                return &field.value;
            }
        }
        return nullptr;
    }
};

inline const Record* Value::as_record() const noexcept {
    const auto* held = std::get_if<std::shared_ptr<Record>>(&data);
    return held == nullptr ? nullptr : held->get();
}

// Same type and same value, for comparing the state of two runs: unlike `==` on floats, two NaNs
// are identical, and 0.0 and -0.0 are not.
[[nodiscard]] bool identical(const Value& a, const Value& b);

// Type name as the IR spells it (`int`, `float`, `bool`, `string`, `list`, `null`).
[[nodiscard]] std::string_view type_name(const Value& value);

// A readable form for diagnostics: ints and floats as in templates, strings quoted, lists in
// brackets.
[[nodiscard]] std::string describe(const Value& value);

} // namespace tessera::compiler::eval
