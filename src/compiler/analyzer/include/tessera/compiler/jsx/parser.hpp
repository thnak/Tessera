// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"
#include "tessera/compiler/jsx/limits.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/token.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::jsx {

// Result of one parse. `root` is null after a syntax error that makes the structure unknowable
// (unclosed element, unexpected token, a limit). It can be present together with error
// diagnostics when the structure was clear and only the subset was violated (a lowercase tag,
// `==`, an unknown entity): the parser keeps going so all independent errors are reported
// (spec section 13.3), but the tree must not be lowered while has_errors() is true.
template<typename Node> struct ParseResult {
    std::unique_ptr<Node> root;
    std::vector<Comment> comments; // in source order, for bind:: annotations
    std::vector<Diagnostic> diagnostics;
    bool diagnostics_truncated = false;

    [[nodiscard]] bool has_errors() const {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.severity == Severity::Error) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool ok() const {
        return root != nullptr && !has_errors();
    }
};

using JsxParseResult = ParseResult<JsxNode>;
using ExpressionParseResult = ParseResult<Expr>;
using ModuleParseResult = ParseResult<Module>;

// Parses a whole file: imports, type aliases, constants and component functions (spec sections 4
// to 7). Recovery is per top-level item (section 14): an error inside one item marks that item
// invalid, the parser skips to the next item start, and the other items are still parsed.
// `root` is null only when the file cannot be read at all (file size limit).
ModuleParseResult parse_module(std::string_view source, const Limits& limits = {});

// Type expression (spec section 5), used by the type tests.
using TypeParseResult = ParseResult<TypeNode>;
TypeParseResult parse_type(std::string_view source, const Limits& limits = {});

// Parses exactly one JSX element or fragment (spec section 10.1) and nothing else.
// `source` must outlive the call only; the result owns everything it returns.
JsxParseResult parse_jsx_element(std::string_view source, const Limits& limits = {});

// Parses exactly one expression (spec section 8, plus the arrow handlers of section 9).
ExpressionParseResult parse_expression(std::string_view source, const Limits& limits = {});

// Deterministic S-expression dumps for golden and determinism tests. Spans are not included.
std::string dump(const Expr& expr);
std::string dump(const JsxNode& node);
std::string dump(const TypeNode& type);
std::string dump(const Module& module);

} // namespace tessera::compiler::jsx
