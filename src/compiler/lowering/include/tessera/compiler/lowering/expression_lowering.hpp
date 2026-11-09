// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "tessera/compiler/lowering/name_resolver.hpp"

#include <expected>
#include <string_view>
#include <vector>

namespace tessera::compiler::lowering {

// Everything lowering needs that the AST does not carry: the source map for spans, the
// project-relative path for `SourceSpan.file`, and the reference resolver.
struct ExpressionLoweringContext {
    const jsx::SourceMap& source_map;
    std::string_view file;               // project-relative, POSIX separators
    const NameResolver* names = nullptr; // null: every reference is TESSERA_IR_UNRESOLVED
};

// The lowerer stops at the first error, so this holds exactly one entry today. It is a vector so
// the collecting pass of a later slice can report more without changing the signature.
using ExpressionLoweringErrors = std::vector<jsx::Diagnostic>;
using ExpressionLoweringResult = std::expected<ir::ExprPtr, ExpressionLoweringErrors>;

// Lowers one pure expression (docs/specs/jsx-subset-v1.md section 8) to the expression shape of
// docs/specs/ir-v1.md section 4: literal, ref, unary, binary, conditional, template and call.
// Anything else is a TESSERA_IR_* error, and a failing subtree yields no IR at all: the expected
// holds diagnostics, never a partially built tree.
//
// `expr` must be non-null and outlive the call; nothing in `context` is retained.
ExpressionLoweringResult lower_expression(const jsx::Expr& expr,
                                          const ExpressionLoweringContext& context);

} // namespace tessera::compiler::lowering
