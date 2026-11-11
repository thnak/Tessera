// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace tessera::compiler::eval {

// Number to string, locale-independent (docs/specs/ir-v1.md section 4.1): an `int` in base 10 with
// a leading `-` for negatives.
[[nodiscard]] std::string int_text(std::int32_t value);

// A `float`: the shortest decimal text that parses back to the same value (the output of C++
// `std::to_chars` without a precision), `NaN`, `Infinity` or `-Infinity`; negative zero prints as
// `-0`.
[[nodiscard]] std::string float_text(double value);

// `formatNumber(x, decimals)` (section 4.2): fixed-point text with `decimals` digits after a `.`
// separator (`decimals` is clamped to 0..20), correctly rounded from the exact value of `x` with
// ties away from zero, no thousands separator, `NaN`, `Infinity` and `-Infinity` as above. A
// negative `x` keeps its sign when it rounds to zero (`-0.00`), as a set sign bit does for `-0`.
[[nodiscard]] std::string fixed_text(double value, std::int64_t decimals);

} // namespace tessera::compiler::eval
