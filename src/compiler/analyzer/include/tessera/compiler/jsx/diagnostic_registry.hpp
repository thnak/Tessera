// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace tessera::compiler::jsx {

// Every stable diagnostic code of the JSX/TSX front end, sorted lexicographically. Codes are
// stable strings (docs/specs/jsx-subset-v1-jsx.md section 13.1): one is never reused for a
// different meaning.
//
// The front end and the Tier-1 rules report the codes that have a fixture in
// tests/unit/jsx_diagnostics_cases.hpp. The remaining entries are owned by a later pipeline stage
// (lowering, schema, project resolver) and are recorded here so the whole front end has one list
// instead of a copy per stage. The registry test (tests/unit/jsx_diagnostics_registry_tests.cpp)
// fails when a code is emitted but missing here, when a listed code has neither a fixture nor an
// explicit skip entry, or when the committed golden tests/golden/jsx/diagnostics-registry.txt
// drifts. Adding or renaming a code therefore requires updating this list, the fixture or skip
// entry, and the golden.
inline constexpr std::string_view kDiagnosticCodes[] = {
    "TESSERA_JSX_CHILDREN_NOT_ALLOWED",
    "TESSERA_JSX_LEX_INVALID_CHARACTER",
    "TESSERA_JSX_LEX_INVALID_ESCAPE",
    "TESSERA_JSX_LEX_INVALID_NUMBER",
    "TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER",
    "TESSERA_JSX_LEX_INVALID_UTF8",
    "TESSERA_JSX_LEX_LONE_CR",
    "TESSERA_JSX_LEX_UNTERMINATED_COMMENT",
    "TESSERA_JSX_LEX_UNTERMINATED_STRING",
    "TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE",
    "TESSERA_JSX_LIMIT_COMPONENTS",
    "TESSERA_JSX_LIMIT_DEPTH",
    "TESSERA_JSX_LIMIT_DIAGNOSTICS",
    "TESSERA_JSX_LIMIT_FILE_SIZE",
    "TESSERA_JSX_LIMIT_TOKENS",
    "TESSERA_JSX_PARSE_EMPTY_EXPRESSION",
    "TESSERA_JSX_PARSE_MISMATCHED_TAG",
    "TESSERA_JSX_PARSE_MISSING_RETURN",
    "TESSERA_JSX_PARSE_MISSING_SEMICOLON",
    "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT",
    "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN",
    "TESSERA_JSX_PARSE_UNKNOWN_ENTITY",
    "TESSERA_JSX_SUBSET_ANIMATED_READ",
    "TESSERA_JSX_SUBSET_ARRAY_METHOD",
    "TESSERA_JSX_SUBSET_ASYNC",
    "TESSERA_JSX_SUBSET_CALL",
    "TESSERA_JSX_SUBSET_CLASS",
    "TESSERA_JSX_SUBSET_COMPONENT_NAME",
    "TESSERA_JSX_SUBSET_DOM",
    "TESSERA_JSX_SUBSET_DYNAMIC_TAG",
    "TESSERA_JSX_SUBSET_FUNCTION_VALUE",
    "TESSERA_JSX_SUBSET_HTML_TAG",
    "TESSERA_JSX_SUBSET_IMPORT",
    "TESSERA_JSX_SUBSET_IMPORT_CYCLE",
    "TESSERA_JSX_SUBSET_LITERAL",
    "TESSERA_JSX_SUBSET_LOOSE_EQUALITY",
    "TESSERA_JSX_SUBSET_MEMBER",
    "TESSERA_JSX_SUBSET_MUTATION",
    "TESSERA_JSX_SUBSET_OPERATOR",
    "TESSERA_JSX_SUBSET_SPREAD",
    "TESSERA_JSX_SUBSET_STATEMENT",
    "TESSERA_JSX_SUBSET_STYLE_DYNAMIC",
    "TESSERA_JSX_SUBSET_TYPES",
    "TESSERA_JSX_SUBSET_UNKNOWN_PROP",
    "TESSERA_JSX_TYPE_MISMATCH",
    "TESSERA_JSX_TYPE_NUMBER",
    "TESSERA_REACT_BIND_ID",
    "TESSERA_REACT_BIND_MISSING",
    "TESSERA_REACT_BIND_POSITION",
    "TESSERA_REACT_CUSTOM_HOOK",
    "TESSERA_REACT_EFFECT_DEPS",
    "TESSERA_REACT_EFFECT_RETURN",
    "TESSERA_REACT_HOOK_FORM",
    "TESSERA_REACT_HOOK_NOT_ALLOWED",
    "TESSERA_REACT_HOOK_ORDER",
    "TESSERA_REACT_LIST_KEY",
    "TESSERA_REACT_NO_PREVIEW_BODY",
    "TESSERA_REACT_PREVIEW_BODY",
    "TESSERA_REACT_SETTER_IN_RENDER",
    "TESSERA_REACT_UNKNOWN_HOOK",
    "TESSERA_REACT_UNSUPPORTED_API",
};

// The same list as a span, for iteration and for tooling that consumes the registry.
inline constexpr std::span<const std::string_view> kDiagnosticCodeRegistry{kDiagnosticCodes};

} // namespace tessera::compiler::jsx
