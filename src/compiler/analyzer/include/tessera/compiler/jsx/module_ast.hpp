// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/type_ast.hpp"

#include <string>
#include <vector>

// Module level of docs/specs/jsx-subset-v1.md sections 4 to 7. Like the rest of the AST it is
// internal to the front end; the IR never includes it.
namespace tessera::compiler::jsx {

enum class ItemKind { Unknown, Import, TypeAlias, Const, Component };

enum class StatementKind {
    Const,      // `const x = e;`, `const [a, b] = e;`: names, optional type, init
    Expression, // `useEffect(...);`: init holds the expression
};

// Hooks are not special here: `const [x, setX] = useState<int>(0);` is a Const statement whose
// init is a Call with type_args. Recognising hooks is the analyzer's job.
struct Statement {
    StatementKind kind = StatementKind::Expression;
    Span span;
    std::vector<std::string> names;
    bool array_pattern = false;
    TypePtr type;
    ExprPtr init;
    // Set by `// Note: bind::<id>` on the line before a handler declaration or a `useEffect`
    // statement (spec 9.3). Empty when the statement has no annotation.
    std::string bind_id;
    Span bind_span;
};

struct PropBinding {
    std::string name;
    Span span;
    ExprPtr default_value; // a literal, or null
};

struct ImportName {
    std::string name;
    Span span;
};

struct ModuleItem {
    ItemKind kind = ItemKind::Unknown;
    Span span;
    // False after any error inside the item; an invalid component produces no IR (spec 14).
    bool valid = true;
    bool exported = false;

    // Import: source and names; side_effect for `import "./x.css";`.
    std::string source;
    std::vector<ImportName> names;
    bool side_effect = false;

    // TypeAlias, Const, Component.
    std::string name;
    Span name_span;
    TypePtr type; // alias target, const annotation, or component return type
    ExprPtr init; // Const

    // Component: zero or one parameter, the body statements, and the single returned element.
    bool has_props = false;
    bool props_destructured = false;
    std::string props_name;
    std::vector<PropBinding> props_bindings;
    TypePtr props_type;
    std::vector<Statement> body;
    JsxPtr returned;
};

struct Module {
    std::vector<ModuleItem> items;
};

} // namespace tessera::compiler::jsx
