// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/type_ast.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Module level of docs/specs/jsx-subset-v1.md sections 4 to 7. Like the rest of the AST it is
// internal to the front end; the IR never includes it.
namespace tessera::compiler::jsx {

// `Hook` is a project-declared `use...` function (docs/specs/custom-hooks-v1.md section 2). It is
// only produced when the experimental `FrontEndOptions::custom_hooks` switch is on; otherwise such
// a function is the `TESSERA_REACT_CUSTOM_HOOK` rejection and is parsed as a component.
//
// `Helper` is a lowercase `function` at module scope (issue #660's corpus): not a component, and
// its body is skipped rather than lowered, so the component beside it compiles.
enum class ItemKind : std::uint8_t {
    Unknown,
    Import,
    ReExport,
    TypeAlias,
    Const,
    Component,
    Hook,
    Helper
};

enum class StatementKind : std::uint8_t {
    Const,      // `const x = e;`, `const [a, b] = e;`: names, optional type, init
    Expression, // `useEffect(...);`: init holds the expression
};

// One entry of an object pattern at a custom-hook call site (docs/specs/custom-hooks-v1.md section
// 3): `const { page, next: goNext } = usePagination(...)`. `key` is the returned field, `local` the
// name the call site introduces (the key unless renamed with `:`).
struct PatternBinding {
    std::string key;
    std::string local;
    Span span;
};

// One element of a `useMemo`/`useCallback` dependency list the parser desugared away (issue #829):
// a plain name and the span it was written at.
struct DependencyName {
    std::string name;
    Span span;
};

// Hooks are not special here: `const [x, setX] = useState<int>(0);` is a Const statement whose
// init is a Call with type_args. Recognising hooks is the analyzer's job.
struct Statement {
    StatementKind kind = StatementKind::Expression;
    Span span;
    std::vector<std::string> names;
    bool array_pattern = false;
    // An object pattern. Two producers, one field:
    //
    //   * `const { state } = io;` (issue #660's corpus) binds each listed field of the initializer
    //     to a local of the same name. The lowering rewrites it to one `const <field> =
    //     <init>.<field>;` per name, so the IR never sees a pattern. A renamed binding
    //     (`{ state: s }`) is not accepted: the subset has no rename form, and inventing one would
    //     put a second naming rule in the language.
    //   * a custom-hook call site (docs/specs/custom-hooks-v1.md section 3), only parsed with the
    //     experimental switch on, where a rename IS allowed because the expansion pass consumes it
    //     and it never reaches the rules or lowering.
    //
    // `key == local` for the first form, so one representation serves both.
    std::vector<PatternBinding> object_pattern;
    TypePtr type;
    ExprPtr init;
    // Set by `// Note: bind::<id>` on the line before a handler declaration or a `useEffect`
    // statement (spec 9.3). Empty when the statement has no annotation.
    std::string bind_id;
    Span bind_span;
    // ADR-066 item 1 and issue #829: the dependency list a `useMemo`/`useCallback` was written
    // with, recorded before the desugar drops it so the rules pass can compare it with the inferred
    // set. `memo_hook` is `useMemo`/`useCallback` when the statement is a desugared memo/callback,
    // empty otherwise. `memo_array` is true when the author wrote an array; an absent one is
    // accepted and the pass knows not to look for it. `memo_deps` holds the plain-name elements in
    // written order; `memo_deps_span` is the whole array literal (the span of the "missing"
    // warning), and `memo_deps_error_span` is the first element that is not a plain name (offset 0,
    // length 0 when none), the span of the hard error.
    std::string memo_hook;
    bool memo_array = false;
    std::vector<DependencyName> memo_deps;
    Span memo_deps_span;
    Span memo_deps_error_span;
};

struct PropBinding {
    std::string name;
    Span span;
    ExprPtr default_value; // a literal, or null
};

// One positional parameter of a custom hook (docs/specs/custom-hooks-v1.md section 2): a name, a
// declared type and an optional literal default. No destructuring, no rest, no `this`. Distinct
// from `HookParam` in hook_manifest.hpp, which describes a *library* hook's parameter.
struct CustomHookParam {
    std::string name;
    Span span;
    TypePtr type;
    ExprPtr default_value; // a literal, or null
};

struct ImportName {
    // `name` is the name in the exporting module (or the local name of a re-export without
    // `from`); `local_name` is the `as` alias when one is written, empty otherwise.
    std::string name;
    Span span;
    std::string local_name;

    // The name this binding introduces: the alias when written, else `name`.
    [[nodiscard]] std::string_view local() const {
        return local_name.empty() ? std::string_view(name) : std::string_view(local_name);
    }
};

// One positional parameter of a module-scope helper (ADR-066 item 4). A helper is not lowered, so
// its parameters are recorded only for the one helper the lowering reads: a reducer, whose
// `(state, action)` names and types are what its `switch` and its record results are keyed by.
struct HelperParam {
    std::string name;
    Span span;
    TypePtr type; // the written annotation; null when the parameter has none
};

struct ModuleItem {
    ItemKind kind = ItemKind::Unknown;
    Span span;
    // False after any error inside the item; an invalid component produces no IR (spec 14).
    bool valid = true;
    bool exported = false;
    // `export default function Name(...)` (ADR-066 item 1): the component is declared under its own
    // name, so the default-ness changes nothing downstream. Recorded so a later pass can report a
    // second default export if it ever matters.
    bool default_export = false;

    // Import: source and names; side_effect for `import "./x.css";`. A re-export (`ItemKind::
    // ReExport`) uses `names` the same way, with `source` empty for `export type { A };`.
    std::string source;
    std::vector<ImportName> names;
    bool side_effect = false;
    // `import styles from "./Card.module.css";` (issue #463): the local name the module's class map
    // is bound to. Empty for every other import. A default import of any other specifier stays
    // TESSERA_JSX_SUBSET_IMPORT.
    std::string css_module_binding;
    // `import type { ... }` / `export type { ... }`: the binding introduces types only, so it adds
    // no runtime import edge (docs/specs/jsx-subset-v1.md section 4).
    bool type_only = false;
    // `import React from "react"` / `import * as React from "react"` (ADR-066 item 1): accepted and
    // ignored, because the automatic JSX runtime means the binding is never read. Recorded so the
    // lowering skips the item instead of resolving a value that does not exist.
    bool ignored_react_import = false;

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

    // Helper only: the positional parameters, in order. A component's single parameter is the
    // `props_*` fields above; the two never apply to the same item.
    std::vector<HelperParam> helper_params;

    // Hook (docs/specs/custom-hooks-v1.md section 2): the positional parameters and the returned
    // value. A hook has no props and no JSX return; `body` holds its statements and `hook_return`
    // the single `return` expression, which is a pure expression, an array literal or an object
    // literal. `returned` stays null for a hook.
    std::vector<CustomHookParam> hook_params;
    ExprPtr hook_return;
};

struct Module {
    std::vector<ModuleItem> items;
};

} // namespace tessera::compiler::jsx
