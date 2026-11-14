# JSX/TSX subset v1

Status: proposed (Big Step C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)).
Decision records: [ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) (own
JSX/TSX front end; React-like model) and [ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md)
(self-contained Wasm designer).

This document is the normative definition of the source language that the Tessera front end accepts.
Anything not accepted here is a compile error (section 12). It does not define what the accepted
constructs mean at run time; that is defined in [`component-model-v1.md`](component-model-v1.md) and
[`ir-v1.md`](ir-v1.md).

## 0. How to read this document

### 0.1 Terms

| Term | Meaning in this document |
|---|---|
| Front end | The Tessera lexer, parser and analyzer that turn source text into an AST and then IR ([`compiler.md`](../design/compiler.md) section 3) |
| AST | Abstract syntax tree: the parser output (section 16). It never reaches code generation |
| IR | The intermediate representation defined in [`ir-v1.md`](ir-v1.md); the only input to code generation |
| Subset | The part of JSX/TSX defined here |
| Component | A function that returns JSX and follows section 6 |
| Intrinsic | A built-in element such as `View` (section 11) |
| Pure expression | An expression with no side effects, from the grammar in section 8 |
| Setter | The second value returned by `useState`, for example `setCount` |
| Handler | A function value passed to an event prop or to an effect (section 9) |
| Native handler | A handler whose run-time behavior is developer-written C++ inside a protected region, announced by a `// Note: bind::<id>` comment (section 9.3). A protected region is a developer-owned block that survives regeneration ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)) |
| Preview body | The TypeScript body of a native handler; the browser designer runs it because it cannot run C++ (section 9.3) |
| Live view | The preview in the browser designer ([`designer.md`](../design/designer.md)) |
| Span | A source location: file, byte offset and length, and 1-based start and end line and column (section 13.2) |
| V1 | The first release; see the [glossary](../reference/glossary.md) |

### 0.2 References

| Reference | Subject |
|---|---|
| [`component-model-v1.md`](component-model-v1.md) | Component, hook, props, event, lifecycle and reconciliation semantics |
| [`ir-v1.md`](ir-v1.md) | IR: node kinds, expressions, loops, bindings, source spans |
| [`compiler.md`](../design/compiler.md) | Compiler stages; "Tier 1" (React rules) and "Tier 2" (bindings and lowering) |
| [`stylesheet-v1.md`](stylesheet-v1.md) | Style properties and values accepted in `style` and `className` |
| [`custom-elements-v1.md`](custom-elements-v1.md) | `CustomElement` and `ui/elements.json` |
| [`virtualization-v1.md`](virtualization-v1.md) | `VirtualList` and `VirtualGrid` |
| [`error-boundaries-v1.md`](error-boundaries-v1.md) | `ErrorBoundary` |
| [`view-transitions-v1.md`](view-transitions-v1.md) | `startViewTransition` update-function rule |
| [`developer-workflow.md`](../guides/developer-workflow.md) | Protected regions and `bind::` ids |

## 1. Purpose and non-goals

Purpose: let React/TypeScript developers write UI in familiar syntax while keeping the language small
enough to parse, analyze statically, lower to IR and run in the designer without a JavaScript
engine ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)).

Non-goals:
- Running arbitrary TypeScript or JavaScript. There is no JavaScript runtime in a production app or
  in the designer ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md)).
- Type checking. Types are parsed and used to lower values to IR types (section 5); the front end
  does not implement TypeScript's type system and never reports TypeScript type errors.
- Compatibility with existing React code. Such code compiles only if it already stays inside this
  subset.

## 2. Source files

- Extensions: `.tsx` and `.jsx`. Both use the same grammar; type annotations in `.jsx` are an error
  (`TESSERA_JSX_SUBSET_TYPES`).
- Encoding: UTF-8. A leading byte order mark is ignored. Invalid UTF-8 is an error.
- Line endings: LF and CRLF are both accepted and are one line break for spans. A lone CR is an
  error.
- Files are discovered by the `include` and `exclude` globs of `tessera.json`
  ([`project-config-v1.md`](project-config-v1.md)).
- One file is parsed without reading other files. Imports are resolved after parsing (section 4).

## 3. Lexical grammar

### 3.1 Whitespace and comments

- Whitespace: space, tab, LF, CR.
- Comments: `// ...` to end of line and `/* ... */` (not nested). Comments are kept with their spans
  because binding annotations are comments (section 9.3).

### 3.2 Identifiers

`IDENT = [A-Za-z_$][A-Za-z0-9_$]*`. Only ASCII is allowed in identifiers. Text inside strings,
templates and JSX text may contain any Unicode.

### 3.3 Keywords

Reserved: `import`, `export`, `from`, `function`, `const`, `return`, `type`, `true`, `false`, `null`,
`as`. Words that are keywords in TypeScript or JavaScript but unused here (for example `class`,
`let`, `var`, `if`, `for`, `while`, `switch`, `try`, `await`, `async`, `new`, `this`, `delete`,
`typeof`, `instanceof`, `yield`) are also reserved so that using one gives a specific diagnostic
(section 12) and not a generic syntax error.

### 3.4 Literals

| Literal | Accepted form | Notes |
|---|---|---|
| Number | decimal integer or decimal with fraction and optional exponent: `0`, `42`, `0.5`, `1e3` | No hex/octal/binary, no numeric separators (`1_000`), no BigInt suffix. Unary minus is an operator, not part of the literal |
| String | `"..."` or `'...'` | Escapes: `\\ \" \' \n \r \t \uXXXX \u{X...}`. A raw line break inside a string is an error |
| Template | `` `text ${expr} text` `` | Each `${}` holds a pure expression; no tagged templates |
| Boolean | `true`, `false` | |
| Null | `null` | Only as a ref initial value and in comparisons allowed by section 8 |

Not accepted: regular-expression literals, `undefined` as a literal (the identifier `undefined` is
rejected), array literals except as defined in section 8.4, object literals except as defined in
section 8.5.

### 3.5 Punctuators

`( ) { } [ ] < > </ /> , ; : . ... => = ? + - * / % == != === !== < <= > >= && || ! `` ` `` `${`.
`==` and `!=` are lexed so they can be rejected with a suggestion to use `===` and `!==`
(section 8.2). All other JavaScript operators (`++ -- += ?? ?. ** & | ^ ~ << >> >>>`, comma operator)
are lexed and rejected (section 12).

### 3.6 Automatic semicolon insertion

Not supported in V1. Every statement ends with `;`. A missing `;` is reported at the end of the
previous token with the suggested fix "insert `;`". This is a design choice to keep the grammar
unambiguous. The project owner decided this.

## 4. Module structure

A file is a sequence of top-level items in any order:

```ebnf
Module        = { Import | ReExport | TypeAlias | ComponentDecl | ConstDecl } ;
Import        = "import" [ "type" ] NamedImports "from" STRING ";"
              | "import" STRING ";" ;
NamedImports  = "{" [ ImportName { "," ImportName } [ "," ] ] "}" ;
ImportName    = IDENT [ "as" IDENT ] ;
ReExport      = "export" "type" "{" [ ImportName { "," ImportName } [ "," ] ] "}"
                [ "from" STRING ] ";" ;
TypeAlias     = [ "export" ] "type" IDENT "=" TypeExpr ";" ;
ComponentDecl = [ "export" ] "function" IDENT "(" [ Param ] ")" [ ":" TypeExpr ] Block ;
ConstDecl     = [ "export" ] "const" IDENT [ ":" TypeExpr ] "=" PureExpr ";" ;
```

Rules:
- **Imports**: named imports only, with an optional `as` rename
  (`import { CardProps as Props } from "./types";`). No default imports, no namespace imports
  (`* as x`), no dynamic `import()`, no `require`. `import type { T } from "./types";` binds type
  names only and adds no runtime import edge (so type-only imports do not form an import cycle).
  - `from "tessera"` imports built-in intrinsics and hooks (for example `useState`, `View`,
    `CustomElement`). Library components such as `Field` and `Form` are not built in: importing one
    from `"tessera"` is `TESSERA_IR_UNSUPPORTED` naming `tessera-ui`. `from "react"` is accepted for `useState`, `useEffect`, `useRef` only, as an alias
    of `"tessera"` (other names and `import React` are errors; [`jsx-intrinsics-v1`](jsx-intrinsics-v1/react-compat.md) Q15).
  - `from "./X"` or `from "../X"` imports a component, type or constant exported by another source
    file of the project (extension omitted). Absolute paths are errors.
  - `from "pkg"` or `from "pkg/Sub"` resolves through a package root configured in `tessera.json`
    `packages` ([`project-config-v1.md`](project-config-v1.md)): `"pkg"` is `<root>/index` and
    `"pkg/Sub"` is `<root>/Sub`, extensions omitted like a relative import. A bare specifier with
    no package root is `TESSERA_IR_UNSUPPORTED` naming the specifier ("neither a project file
    (`./X`) nor a configured package"); a configured root without the named file is
    `TESSERA_IR_UNRESOLVED`. `"tessera"` and `"react"` stay built in and are never captured by a
    package root of the same name. A package entry that declares a `version` is checked against the
    root's `tessera-package.json` before compilation (`TESSERA_PACKAGE_VERSION`,
    `TESSERA_PACKAGE_MANIFEST`; [`project-config-v1.md`](project-config-v1.md) `packages`). The
    component library is imported this way: `from "tessera-ui"` and `from "tessera-ui/<Name>"`
    resolve inside the one `tessera-ui` root ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)).
    When the project has no `tessera-ui` entry, those specifiers resolve to the library bundled
    with the compiler (`Field`, `Form`); an unknown name or subpath is `TESSERA_IR_UNRESOLVED`. A
    configured `tessera-ui` entry replaces the bundled library with its own files.
  - **An imported component shadows an intrinsic element of the same name in that file** (issue
    #291). `import { Button } from "tessera-ui"` makes `<Button>` the library component; a file
    without the import keeps the intrinsic `Button`, and a renamed import (`import { Button as
    Action }`) shadows under its local name only. A component declared in the same file does not
    shadow an intrinsic. The library component renders intrinsic elements (`View`) itself, so the
    intrinsic stays reachable.
  - A resolved package import follows the ordinary cross-file rules: the imported name must be
    declared in the target file, component names stay unique project-wide, import cycles apply,
    and the package file is compiled by the same analyzer, rules and lowering pipeline (no
    special-casing downstream).
  - `import "./file.css";` (side-effect import) registers a stylesheet with the build
    ([`stylesheet-v1.md`](stylesheet-v1.md)); the path must end in `.css`. Lowering resolves it
    relative to the importing file and records it in `IrModule::stylesheets` (`id`, `path`,
    `hash`; [`ir-v1.md`](ir-v1.md) section 1). A missing or unreadable file is
    `TESSERA_IR_STYLESHEET_MISSING` at the import span. Generated code does not embed the CSS
    yet, so codegen warns `TESSERA_CG_STYLESHEET_NOT_EMBEDDED` naming the file (apply it with
    `compile_stylesheet()` or `tessera.json` `ui.styles` until embedding lands).
- **Exports**: named exports only. `export default` is rejected in V1.
- **Type re-exports**: `export type { A, B as C } [from "./X"];` re-exports type names, from another
  file or from the file's own imports. Other re-exports (`export { A }`, `export *`) are rejected. A
  re-exported name resolves like a directly declared type alias, so a props alias may be reached
  through a chain of re-exports and is lowered by the same rules as an inline alias.
- **Components** are `function` declarations (section 6). Arrow-function components and anonymous
  components are rejected ([`component-model-v1.md`](component-model-v1.md) section 8).
- **Top-level constants** (`ConstDecl`) must have a pure expression made of literals and other
  top-level constants. They are evaluated at compile time.
- **Cycles**: an import cycle between files is an error (`TESSERA_JSX_SUBSET_IMPORT_CYCLE`).
- No top-level statements other than the items above.

## 5. Types

Type syntax is parsed so that source looks like TypeScript, and is used to choose IR types.

```ebnf
TypeExpr   = UnionType ;
UnionType  = PrimaryType { "|" PrimaryType } ;
PrimaryType= "number" | "boolean" | "string" | "null"
           | STRING                                  (* string literal type *)
           | IDENT [ "<" TypeExpr { "," TypeExpr } ">" ]   (* alias or built-in *)
           | PrimaryType "[]"
           | "{" [ Member { ( ";" | "," ) Member } [ ";" | "," ] ] "}"
           | "(" [ ParamType { "," ParamType } ] ")" "=>" TypeExpr ;
Member     = IDENT [ "?" ] ":" TypeExpr ;
ParamType  = IDENT ":" TypeExpr ;
```

- **Numeric types.** Written types are `int` and `float`, both imported from `"tessera"` as type
  aliases of TypeScript `number` (so `tsc` and editors accept them). They lower one to one to the IR
  types `int` and `float` ([`ir-v1.md`](ir-v1.md) section 2). A bare `number` in a state type, prop
  type, handler parameter, return type or record member is rejected with
  `TESSERA_JSX_TYPE_NUMBER` and the suggestion to write `int` or `float`. The project owner decided this.
- **Literal types.** A number literal without a fraction or exponent (`0`, `42`) is `int`, unless the
  expected type at that position is `float`, in which case it is `float`. A literal with a fraction or
  exponent (`0.5`, `1e3`) is `float`; it is an error where `int` is expected
  (`TESSERA_JSX_TYPE_MISMATCH`).
- **Arithmetic types.** `int` widens to `float` implicitly. `float` never narrows to `int` implicitly.
  `+ - *` and unary `-` on two `int` give `int`, otherwise `float`. `/` always gives `float`. `%` on two
  `int` gives `int`, otherwise `float`. Integer width, overflow, division by zero and number
  formatting are defined by [`ir-v1.md`](ir-v1.md) section 4.1, not here.
- **`Children`.** The type of the prop named `children` (`type CardProps = { children: Children }`),
  imported from `"tessera"`. It is the only type of `children`, is never optional, and has no value
  expression: `{children}` is accepted only as a child of an element, once per component, not in a
  loop, and lowers to the IR `Slot` node ([`ir-v1.md`](ir-v1.md) section 3.4). A `Children` value
  cannot be stored, compared, passed to another prop or used in text.
- A union is accepted only as `T | null` or as a union of string literals (an enum).
- Type aliases are non-recursive and may refer only to earlier or later aliases without cycles,
  including aliases imported from other files (a cycle is `TESSERA_IR_TYPE_CYCLE`). A component's
  props type may be an alias declared in another project file (section 4).
- Built-in generic names accepted: `Ref<T>` is not written by developers; refs use the element type
  directly (`useRef<Text>(null)`, section 7). Element types are the intrinsic names of section 11.
- Not accepted: `any`, `unknown`, `never`, `object`, `Function`, intersections (`&`), conditional or
  mapped types, `keyof`/`typeof` types, tuple types, enums (`enum`), interfaces (`interface`),
  generics on user declarations, `as` assertions, non-null `!` assertions, `satisfies`, decorators.

## 6. Components

```ebnf
Param  = ( IDENT | DestructuredProps ) ":" TypeExpr ;
DestructuredProps = "{" PropBind { "," PropBind } [ "," ] "}" ;
PropBind = IDENT [ "=" PropDefault ] ;
PropDefault = Literal | "[" [ Literal { "," Literal } ] "]" ;
Block  = "{" { Stmt } ReturnStmt "}" ;
Stmt   = StateDecl | RefDecl | AnimDecl | EffectStmt | HandlerDecl | LocalDecl ;
ReturnStmt = "return" ( JsxElement | "(" JsxElement ")" ) ";" ;
LocalDecl  = "const" IDENT [ ":" TypeExpr ] "=" PureExpr ";" ;
```

- A component has zero or one parameter: the props. The parameter is either a name
  (`props: CounterProps`, read as `props.initial`) or a destructuring with literal or flat
  literal-list defaults (`{ initial = 0 }: CounterProps`, `{ items = [] }: P`). The type annotation
  is required when the parameter exists. Rest elements (`...rest`) are rejected.
- The name starts with an uppercase letter.
- The body is a sequence of statements followed by exactly one `return` that is the last statement.
  The returned value is one JSX element (not an array, string or `null`).
  Early returns, `if`/`switch` statements, loops, `try`, and assignments are rejected;
  conditional output uses `&&` or `?:` inside JSX (section 10.4).
- Statements before `return` may only be hook declarations (section 7), handler declarations
  (section 9) and `LocalDecl` constants. A `LocalDecl` is a pure expression recomputed on each
  update; it may not call hooks.
- Hooks must appear before any other statement kind that depends on them, are unconditional, and
  keep a fixed order; the checks are in section 7 and in
  [`compiler.md`](../design/compiler.md) section 4.
- Props are read-only. Writing to a prop or state variable directly is rejected; state changes use
  setters only.
- `key` and `ref` are not props of a component; they are element attributes (section 10.2).

## 7. Hooks

Hook calls are statements of these exact forms. Argument expressions are pure expressions.

| Statement | Accepted form | Notes |
|---|---|---|
| State | `const [x, setX] = useState<T>(init);` | Exactly two names. `<T>` may be omitted when `init` is a string, boolean or `null` literal or a prop with a declared type; it is required when `init` is a number literal `init` must be pure |
| Ref | `const r = useRef<ElementType>(null);` | `ElementType` is an intrinsic name (section 11). The initial value must be `null` |
| Animated value | `const v = useAnimatedValue(init);` | `init` is a number literal or a pure expression of numbers. Use in handlers is `v.set(expr)` and `v.set(expr, { animate, duration, easing, spring, onComplete })`: `animate` is a boolean, `duration` a non-negative number literal in milliseconds, `easing` a string literal ([`ir-v1.md`](ir-v1.md) section 2.1 lists the accepted easings) and `spring` is `true` or an object literal of optional `stiffness`, `damping` and `mass` number literals (defaults 180, 20, 1; exclusive with `duration`/`easing`), `onComplete` is the name of a parameterless inline setter handler (section 9.2) declared anywhere in the same component, before or after the call (an undeclared name, a native `bind::` handler, a handler with parameters, a setter or any non-identifier is `TESSERA_IR_UNSUPPORTED`); other options, non-literals and repeated options are `TESSERA_IR_UNSUPPORTED`. See [`animation.md`](../design/animation.md) section 6. Reading the value (`v.value` or any other member) is rejected with `TESSERA_JSX_SUBSET_ANIMATED_READ`; an animated value is passed whole as a prop or style value. The project owner decided this |
| Effect | `useEffect(() => { ... }, [dep, ...]);` | The body is a native handler body (section 9.3). Dependencies are names of props, state variables or animated values; an expression is not allowed. Exact matching is enforced as in [`component-model-v1.md`](component-model-v1.md) section 2 |

- Library hooks declared in the built-in hook manifest (section 7.1) are also accepted. Any other name
  starting with `use` gives `TESSERA_REACT_UNKNOWN_HOOK`. A hook that exists (core or manifest) but is not
  listed in `ui.hooks.allowed` gives `TESSERA_REACT_HOOK_NOT_ALLOWED`: an error when `ui.hooks.strict` is
  `true` (the default), a warning when it is `false`; in both cases the hook is still lowered normally
  ([`project-config-v1.md`](project-config-v1.md)).
- A function whose name starts with `use` and that is declared in the project is a custom hook and is
  rejected (`TESSERA_REACT_CUSTOM_HOOK`).
- A hook called inside a condition, a handler, an effect, a loop callback or a nested function is an
  error (`TESSERA_REACT_HOOK_ORDER`).
- The returned value of `useEffect` is not used. An effect may have a cleanup function, written as the
  last statement of the effect body: `return () => { ... };`. The cleanup is a second native handler:
  its protected region id is the effect's bind id followed by `_cleanup` (effect `bind::sync` has
  cleanup `bind::sync_cleanup`), and the IR effect has an optional `cleanup` field holding that id
  ([`ir-v1.md`](ir-v1.md) section 2). The cleanup runs before the effect runs again and on unmount
  ([`component-model-v1.md`](component-model-v1.md) section 2). Its arrow body is its preview body
  (section 9.3). A `return` in any other position of an effect body is `TESSERA_REACT_EFFECT_RETURN`;
  an id that collides with another bind id is `TESSERA_REACT_BIND_ID`. The project owner decided this.

### 7.1 Built-in hook manifest

The **built-in hook manifest** is a JSON file shipped with the Tessera framework that lists every library
hook (a hook other than the four core hooks above). The front end reads it to validate calls and to lower
them to IR; the build also generates the editor typings of section 16.2 from it. A project cannot add
entries: hooks written by app developers are rejected (`TESSERA_REACT_CUSTOM_HOOK`). The source file is
`src/compiler/frontend/hooks-manifest.json` in the framework repository. The build embeds it in the front end
(the native compiler and the Wasm designer both contain it, so neither reads it from disk at run time) and
installs a copy at `share/tessera/hooks-manifest.json` for the typings generator of section 16.2.

```json
{
  "schemaVersion": 1,
  "hooks": [
    {
      "name": "useBreakpoint",
      "params": [],
      "returns": { "ts": "\"compact\" | \"medium\" | \"expanded\"", "ir": "string" },
      "lowering": { "kind": "environment", "provider": "breakpoint" }
    },
    {
      "name": "useThemeState",
      "params": [],
      "returns": { "ts": "[string, (id: string) => void]", "ir": "string" },
      "lowering": { "kind": "environment", "provider": "theme", "writable": true }
    }
  ]
}
```

| Field | Type | Meaning |
|---|---|---|
| `schemaVersion` | integer | `1`. Any other value makes the front end refuse to start (`TESSERA_JSX_MANIFEST_VERSION`) |
| `hooks[].name` | string | Hook name; starts with `use` followed by an uppercase letter; unique; must not equal a core hook name |
| `hooks[].params` | array | Parameters in order, each `{ "name": string, "ts": string, "ir": string }`. `ts` is a type of section 5; `ir` is an IR type of [`ir-v1.md`](ir-v1.md) section 2. V1 library hooks have no parameters |
| `hooks[].returns` | object | `ts`: the type seen by the analyzer; `ir`: the IR type of the value. For a `writable` hook, `ts` is a pair of the value and its setter |
| `hooks[].lowering` | object | How the call lowers to IR. The only kind in V1 is `environment` (below), with `provider` and optional `writable` (boolean, default `false`) |

Call form: `const name = useHook();` (a single `const` binding of the whole value, no destructuring, at the
top level of the component body). A `writable` hook uses the pair form `const [name, setName] = useHook();`
(exactly two names, as for `useState`). The same placement and ordering rules as for core hooks apply
(`TESSERA_REACT_HOOK_ORDER`). The value may be used in any pure expression, in JSX, and as an effect
dependency.

**`environment` lowering.** The call becomes an entry in the IR component field `environment`
([`ir-v1.md`](ir-v1.md) section 2): `{ "id": "env::<name>", "name": "<name>", "provider": "<provider>",
"type": "<ir type>" }`, and every use of the variable becomes a `ref` to that id. A **provider** is an
engine-owned source of a read-only value. When the provider's value changes, the engine marks every
component that reads it as updated, exactly as for a state change
([`component-model-v1.md`](component-model-v1.md) section 5). A component can write an environment value only through the setter of a `writable` hook; the setter is used
like a state setter in handlers of section 9.2 and in `startViewTransition` update functions, and lowers to
a write of `env::<name>`. Other environment values are read-only.

V1 library hooks (the full contents of the V1 manifest):

| Hook | Returns | Value |
|---|---|---|
| `useTheme()` | `string` | The id of the active theme. The engine always provides a built-in theme with id `default` (no tokens); it is the active theme at start unless the project's theme asset (`assets.theme` in [`project-config-v1.md`](project-config-v1.md)) names another start theme ([`stylesheet-v1.md`](stylesheet-v1.md) section 9.1). Never empty. The project owner decided this |
| `useThemeState()` | `[string, (id: string) => void]` | The pair of the active theme id (the same value as `useTheme()`) and a setter. `setTheme(id)` makes `id` the active theme: the stylesheet engine re-resolves styles, as for any theme change ([`stylesheet-v1.md`](stylesheet-v1.md) section 9). A string literal that is not a theme id of the project (`default` plus the keys of the theme asset) is `TESSERA_JSX_THEME_UNKNOWN`; a non-literal unknown id at run time changes nothing and reports the runtime diagnostic `TESSERA_IR_THEME_UNKNOWN` |
| `useBreakpoint()` | `"compact" \| "medium" \| "expanded"` | The size class of the window width in dp (logical pixels): `compact` below 600, `medium` from 600 to 839, `expanded` from 840 upward; the same thresholds as [`responsive.md`](../design/responsive.md) section 2. Uses the window, not the display, and updates on resize, fold change and multi-window changes |

In the browser designer, the values come from the viewer environment set by `viewport.set`
([`designer-protocol-v1.md`](designer-protocol-v1.md) section 5). Adding a hook, changing a signature or a
threshold is a change to this specification, the manifest, the evaluator and the code generator together.

## 8. Expressions

### 8.1 Grammar

```ebnf
PureExpr   = Cond ;
Cond       = Or [ "?" PureExpr ":" PureExpr ] ;
Or         = And { "||" And } ;
And        = Equality { "&&" Equality } ;
Equality   = Relational { ( "===" | "!==" ) Relational } ;
Relational = Additive { ( "<" | "<=" | ">" | ">=" ) Additive } ;
Additive   = Mul { ( "+" | "-" ) Mul } ;
Mul        = Unary { ( "*" | "/" | "%" ) Unary } ;
Unary      = [ "!" | "-" | "+" ] Postfix ;
Postfix    = Primary { "." IDENT | "(" [ PureExpr { "," PureExpr } ] ")" } ;
Primary    = NUMBER | STRING | TEMPLATE | "true" | "false" | "null"
           | IDENT | "(" PureExpr ")" | ArrayLit | ObjectLit ;
```

Operator precedence and left associativity follow JavaScript for the operators listed.

### 8.2 Mapping to IR expressions

| Source | IR expression ([`ir-v1.md`](ir-v1.md) section 4) |
|---|---|
| literal | `literal` |
| identifier (prop, state, local, loop variable, top-level constant) | `ref` |
| `a + b`, `-`, `*`, `/`, `%`, `<`, `<=`, `>`, `>=`, `&&`, `\|\|` | `binary` |
| `===`, `!==` | `binary` with `==`, `!=` (the typed IR has no implicit conversions) |
| `!a`, `-a`, `+a` | `unary` |
| `c ? a : b` | `conditional` |
| template literal | `template` |
| `f(args)` where `f` is on the pure-function allowlist | `call` |
| `a.b` where `a` is a props object, a record-typed state, or a loop item | `ref` with a dotted id (for example `item.id`) |

- `==` and `!=` are rejected with the suggestion "use `===`/`!==`" (`TESSERA_JSX_SUBSET_LOOSE_EQUALITY`).
- Operands must have compatible IR types; a mismatch is `TESSERA_JSX_TYPE_MISMATCH`. There is no
  implicit string/number coercion: `"a" + 1` is an error.
- The pure-function allowlist is defined in [`ir-v1.md`](ir-v1.md) section 4.2. A function is used by
  importing its name from `"tessera"` and calling it; calling any other function in a pure
  expression is `TESSERA_JSX_SUBSET_CALL`.
- Member access `a.b` is accepted only when the front end knows the record type of `a` (props type,
  type alias, loop item type). Access on any other value is `TESSERA_JSX_SUBSET_MEMBER`. Indexing
  (`a[i]`) is rejected except where the loop form of section 10.5 defines it.

### 8.3 Evaluation properties

- No side effects, no assignment, no closures, no function values except those in section 9.
- Evaluation order is left to right; `&&`, `||` and `?:` short-circuit.
- Integer overflow, division by zero and number formatting follow [`ir-v1.md`](ir-v1.md) section 4.1.

### 8.4 Array literals

Accepted only as a `useState` initial value of a declared array type (`useState<string[]>([])`,
`useState<int[]>([1, 2])`) — elements must be literals of the declared element type; a dynamic
element, a nested array, a record element type and a scalar initial for a list type are rejected as
`TESSERA_IR_UNSUPPORTED` (`src/compiler/lowering/src/hook_lowering.cpp`). A list-loop source and a
top-level constant list are not accepted yet: a loop source must be a list reference.

### 8.5 Object literals

Accepted only as: the `style` attribute value (section 10.3), the `props` attribute of
`CustomElement` ([`custom-elements-v1.md`](custom-elements-v1.md)), the `initial` attribute of library
`Form` components, and a `useState` initial value of a declared record type. Keys are identifiers or
string literals (no computed keys, no shorthand with a function value, no spread).

## 9. Handlers

A handler is a function value given to an event prop, to `useEffect`, or to a callback prop such as
`onError`. Exactly three forms are accepted; a callback prop of the component itself may also be
forwarded as an event handler (section 9.5).

### 9.1 Setter reference

The name of a state setter: `onChange={setEmail}`. The setter receives the event's primary value (the
`value` of a text event, otherwise the whole payload) and its type must match the state type
([`jsx-intrinsics-v1`](jsx-intrinsics-v1/common-and-events.md) section 3).

### 9.2 Inline setter handler

An arrow function whose body is a sequence of calls to setters, animated-value `.set(...)` and ref
`r.focus()` / `r.blur()` (no arguments, `r` from `useRef`; issue #226), with pure argument
expressions, separated by `;`:

```tsx
<Button onClick={() => { setCount(count + 1); setLast("increment"); }}>Increment</Button>
<Button onClick={() => setCount(count + 1)}>Increment</Button>
```

- Parameters are allowed and typed by the event (section 9.4); they may be used in the arguments.
- A statement may be guarded: `guard && setter(...)` runs the call only when the pure bool `guard`
  holds (issue #236). A key handler uses it as `(e) => e.key === "Escape" && setOpen(false)`; the
  `KeyEvent` `key` is compared with a W3C key-value string literal (`"Escape"`, `"ArrowDown"`, `" "`,
  a letter). It lowers to the IR action `guard`, and the generated code tests it with
  `tessera::ui::key_is`. A guard on a non-setter call is rejected.
- No other statements: no `if`, no loops, no declarations, no calls other than setters, `.set`, `r.focus()`, `r.blur()` and calls of the component's own callback props (section 9.5).
- The front end lowers this form to IR and generates C++ for it. No protected region is created, and
  the designer runs it exactly as the generated code would.
- `startViewTransition(update, options)` accepts only a handler of this form as `update`
  ([`view-transitions-v1.md`](view-transitions-v1.md) section 3.1).

### 9.3 Native handler

A handler that runs developer-written C++ in production. It is a `const` arrow function declared in
the component body and preceded on the previous line by the comment `// Note: bind::<id>`:

```tsx
// Note: bind::on_submit
const submit = (values: FormValues): void => {
  setStatus("saving");
};
...
<Form onSubmit={submit} />
```

- `<id>` is `[a-z][a-z0-9_]*`, unique in the project; duplicates and malformed ids are errors
  (`TESSERA_REACT_BIND_ID`). The id becomes the protected region id
  ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)).
- Every parameter has a type annotation (section 5 types only). The return type annotation is
  required when the handler returns a value and may be omitted for a handler that returns nothing
  (`void`).
- In production, behavior is the developer's C++ in the protected region `bind::<id>`. The TSX body is
  not translated to C++.
- The TSX body is the **preview body**: the browser designer runs it (it cannot run C++). It follows
  the rules of section 9.2, and may additionally contain `return <PureExpr>;` for a non-`void` handler.
  The preview body is optional. A native handler with an empty body is allowed; the designer then does
  nothing for that event and reports the warning `TESSERA_REACT_NO_PREVIEW_BODY` (severity `warning`,
  never an error, and it does not stop code generation). The project owner decided this. A body that
  is not setter calls plus an optional final `return` is `TESSERA_REACT_PREVIEW_BODY`, also a warning:
  it has no preview and the designer does nothing for the event.
- The compiler does not check the preview body against the C++ code; keeping them equivalent is the
  developer's responsibility. A project can compare them with the preview-parity test: a script of
  handler calls runs on the IR evaluator and on the generated component, and the state must match
  after every step ([`designer.md`](../design/designer.md) section 2, `tests/parity/`).
- The annotation may also precede a hook call to give its protected region a stable id; the accepted
  positions are: before a handler declaration and before a `useEffect` statement. Any other position
  is `TESSERA_REACT_BIND_POSITION`.

### 9.4 Event parameter types

Event names and payload types are those listed in [`component-model-v1.md`](component-model-v1.md)
section 4 and in the element specs ([`text-editing-v1.md`](text-editing-v1.md),
[`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md)). The typed event objects are declared by the
framework typings generated for `"tessera"` (section 16.2). The payload records and which member is
primary are defined in [`jsx-intrinsics-v1`](jsx-intrinsics-v1/common-and-events.md) section 2. An
event prop given a function with a different parameter type is `TESSERA_JSX_TYPE_MISMATCH`.

### 9.5 Callback prop forwarding

A component may declare a function-typed prop and hand it on (issue #286). The prop type is
`() => void` or `(e: Record) => void`, where `Record` is a payload record
([`jsx-intrinsics-v1`](jsx-intrinsics-v1/common-and-events.md) section 2):

```tsx
export type ButtonProps = { onPress: () => void; children: Children };
export function Button({ onPress, children }: ButtonProps) {
  return <View role="button" tabIndex={0} onClick={onPress}>{children}</View>;
}
```

- `onClick={onPress}` with `onPress` a destructured callback prop of the same component lowers to one
  handler with a single IR `callProp` action ([`ir-v1.md`](ir-v1.md) section 6). Nothing is captured:
  the generated code calls the prop's `std::function` member, bound once by the caller, and skips an
  unset one. There is no allocation per event.
- A `() => void` prop forwards to any event, because the call ignores the payload. A payload prop
  forwards only to an event that delivers that same record; any other pairing is
  `TESSERA_IR_UNSUPPORTED` naming both records.
- A prop that is not a callback (or whose type is unknown) is `TESSERA_IR_UNSUPPORTED`; a name that is
  not a prop, setter or handler stays `TESSERA_IR_UNRESOLVED`.
- On a component reference (`<Inner onPress={onPress} />`) only a `() => void` prop is handed on: the
  child's payload record is not known to the lowering yet.
- A callback prop may also be called as a statement of an inline handler (section 9.2), after or
  between setter calls: `() => { setPressed(false); onPress(); }`, `(e) => { setValue(e.value); onChange(e); }`.
  A `() => void` prop is called with no argument. A payload prop is called with the handler's own
  parameter, the whole payload and nothing else (`onChange(e)`, not `onChange(e.value)`), and the
  parameter's record must be the prop's. Anything else is `TESSERA_IR_UNSUPPORTED`. The call lowers to
  the same `callProp` action, runs in order with the other statements and may carry a `guard`.

## Continued in part 2

Sections 10-18 live in [`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md); numbering is unchanged.

| Section | Topic |
|---|---|
| 10 | JSX: elements, attributes, `className`/`style`, children, lists, event props |
| 11 | Intrinsic elements (`Image`, `Path`, `Spacer`) |
| 12 | Rejected constructs |
| 13 | Diagnostics |
| 14 | Error-tolerant parsing (live view) |
| 15 | Determinism and limits |
| 16 | Front-end output |
| 17 | Tests |
| 18 | Open questions |
