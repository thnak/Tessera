// SPDX-License-Identifier: MIT

// The strict whole-string number parse behind `parseIntOr`/`parseFloatOr` (issue #803,
// ir-v1-expr.md section 4.2): the runtime rule the generated C++ calls. These pins are the C++
// half of what `ir_eval_tests` pins for the evaluator, so the two cannot drift apart.

#include "tessera/ui/scene/list_ops.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_int_parse() {
    check(tessera::ui::parse_number_or<int>("42", 0) == 42, "a plain int");
    check(tessera::ui::parse_number_or<int>("  -7  ", 0) == -7, "surrounding ASCII whitespace");
    check(tessera::ui::parse_number_or<int>("+13", 0) == 13, "a plus sign");
    check(tessera::ui::parse_number_or<int>("12abc", -1) == -1,
          "whole-string: a trailing suffix falls back");
    check(tessera::ui::parse_number_or<int>("1.5", 0) == 0, "no decimal point in the int form");
    check(tessera::ui::parse_number_or<int>("1e3", 0) == 0, "no exponent");
    check(tessera::ui::parse_number_or<int>("0x10", 0) == 0, "no hex");
    check(tessera::ui::parse_number_or<int>("", 9) == 9, "the empty string falls back");
    check(tessera::ui::parse_number_or<int>("   ", 9) == 9, "whitespace alone falls back");
    check(tessera::ui::parse_number_or<int>("-", 9) == 9, "a lone sign falls back");
    check(tessera::ui::parse_number_or<int>("--5", 9) == 9, "a doubled sign falls back");
    check(tessera::ui::parse_number_or<int>("2147483648", 0) == std::numeric_limits<int>::max(),
          "above the int range saturates high");
    check(tessera::ui::parse_number_or<int>("-2147483649", 0) == std::numeric_limits<int>::min(),
          "below the int range saturates low");
    check(tessera::ui::parse_number_or<int>("99999999999999999999", 0) ==
              std::numeric_limits<int>::max(),
          "far out of range saturates rather than wrapping");
}

void test_float_parse() {
    check(tessera::ui::parse_number_or<double>("1.5", 0.0) == 1.5, "a plain float");
    check(tessera::ui::parse_number_or<double>("42", 0.0) == 42.0, "an int text is a float too");
    check(tessera::ui::parse_number_or<double>("-0.25", 1.0) == -0.25, "a negative fraction");
    check(tessera::ui::parse_number_or<double>("1e3", 0.0) == 0.0, "no exponent");
    // `.5` and `5.` are both valid float spellings: the rule requires at least one digit anywhere
    // in the body, not one on each side of the point.
    check(tessera::ui::parse_number_or<double>(".5", 1.0) == 0.5, "a leading point is a number");
    check(tessera::ui::parse_number_or<double>("5.", 1.0) == 5.0, "a trailing point is a number");
}

} // namespace

int main() {
    test_int_parse();
    test_float_parse();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "parse_number_or tests passed\n";
    return 0;
}
