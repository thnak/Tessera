// SPDX-License-Identifier: MIT

#include "tessera/compiler/eval/number_text.hpp"
#include "tessera/compiler/eval/value.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <utility>

namespace tessera::compiler::eval {

Value Value::null() {
    return Value{Null{}};
}
Value Value::of_bool(bool value) {
    return Value{value};
}
Value Value::of_int(std::int32_t value) {
    return Value{value};
}
Value Value::of_float(double value) {
    return Value{value};
}
Value Value::of_string(std::string value) {
    return Value{std::move(value)};
}
Value Value::of_list(List value) {
    return Value{std::move(value)};
}

double Value::number() const noexcept {
    if (const std::int32_t* integer = as_int()) {
        return static_cast<double>(*integer);
    }
    if (const double* number = as_float()) {
        return *number;
    }
    return 0.0;
}

bool identical(const Value& a, const Value& b) {
    if (a.data.index() != b.data.index()) {
        return false;
    }
    if (const double* left = a.as_float()) {
        const double right = *b.as_float();
        if (std::isnan(*left) || std::isnan(right)) {
            return std::isnan(*left) && std::isnan(right);
        }
        return std::bit_cast<std::uint64_t>(*left) == std::bit_cast<std::uint64_t>(right);
    }
    if (const List* left = a.as_list()) {
        const List& right = *b.as_list();
        if (left->size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left->size(); ++i) {
            if (!identical((*left)[i], right[i])) {
                return false;
            }
        }
        return true;
    }
    if (const std::int32_t* left = a.as_int()) {
        return *left == *b.as_int();
    }
    if (const bool* left = a.as_bool()) {
        return *left == *b.as_bool();
    }
    if (const std::string* left = a.as_string()) {
        return *left == *b.as_string();
    }
    return true; // both null
}

std::string_view type_name(const Value& value) {
    if (value.as_bool() != nullptr) {
        return "bool";
    }
    if (value.as_int() != nullptr) {
        return "int";
    }
    if (value.as_float() != nullptr) {
        return "float";
    }
    if (value.as_string() != nullptr) {
        return "string";
    }
    if (value.as_list() != nullptr) {
        return "list";
    }
    return "null";
}

std::string describe(const Value& value) {
    if (const bool* flag = value.as_bool()) {
        return *flag ? "true" : "false";
    }
    if (const std::int32_t* integer = value.as_int()) {
        return int_text(*integer);
    }
    if (const double* number = value.as_float()) {
        return float_text(*number);
    }
    if (const std::string* text = value.as_string()) {
        return "\"" + *text + "\"";
    }
    if (const List* list = value.as_list()) {
        std::string out = "[";
        for (std::size_t i = 0; i < list->size(); ++i) {
            out += i == 0 ? "" : ", ";
            out += describe((*list)[i]);
        }
        return out + "]";
    }
    return "null";
}

} // namespace tessera::compiler::eval
