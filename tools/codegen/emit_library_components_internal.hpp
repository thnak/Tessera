// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"

#include <string>
#include <string_view>

// Shared text helpers of the build-time library component IR writer (issue #70). Kept out of the
// public writer header so the expression encoder and the declaration writer can be separate
// translation units.
namespace tessera::compiler::codegen::cli::internal {

// A C++ string literal body with quotes, backslashes and control bytes escaped.
[[nodiscard]] std::string escape_string(std::string_view text);

// `escape_string` wrapped in double quotes.
[[nodiscard]] std::string quoted(std::string_view text);

// Project-relative POSIX path, so generated text never contains a checkout location.
[[nodiscard]] bool is_project_relative(std::string_view file);

// The C++ expression that rebuilds one IR expression. False and `error` when the expression has
// no generated form.
[[nodiscard]] bool expr_source(const ir::Expr& expr, std::string& out, std::string& error);

} // namespace tessera::compiler::codegen::cli::internal
