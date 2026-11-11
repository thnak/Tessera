// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/evaluator.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace tessera::compiler::eval::detail {

[[nodiscard]] std::unexpected<EvalError> fail(std::string code, std::string message);

// `+ - * / % == != < <= > >=` on two evaluated operands. `&&` and `||` short-circuit, so the
// evaluator handles them. `modulo_zero` is set when an `int % 0` produced the defined result 0.
[[nodiscard]] std::expected<Value, EvalError> apply_binary(std::string_view op, const Value& left,
                                                           const Value& right, bool& modulo_zero);

// `!`, `-` and `+`.
[[nodiscard]] std::expected<Value, EvalError> apply_unary(std::string_view op, const Value& value);

// The allowlisted pure functions of ir-v1 section 4.2.
[[nodiscard]] std::expected<Value, EvalError> call_function(std::string_view name,
                                                            std::span<const Value> args);

// The text a template substitutes for a value (ir-v1 section 4.1); only strings and numbers have
// one.
[[nodiscard]] std::expected<std::string, EvalError> template_text(const Value& value);

} // namespace tessera::compiler::eval::detail
