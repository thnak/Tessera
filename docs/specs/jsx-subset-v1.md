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
Module        = { Import | TypeAlias | ComponentDecl | ConstDecl } ;
Import        = "import" ( NamedImports | "{" "}" ) "from" STRING ";"
              | "import" STRING ";" ;
NamedImports  = "{" IDENT { "," IDENT } [ "," ] "}" ;
TypeAlias     = [ "export" ] "type" IDENT "=" TypeExpr ";" ;
ComponentDecl = [ "export" ] "function" IDENT "(" [ Param ] ")" [ ":" TypeExpr ] Block ;
ConstDecl     = [ "export" ] "const" IDENT [ ":" TypeExpr ] "=" PureExpr ";" ;
```

Rules:
- **Imports**: named imports only. No default imports, no namespace imports (`* as x`), no
  re-exports, no dynamic `import()`, no `require`.
  - `from "tessera"` imports built-in components and hooks (for example `useState`, `View`,
    `CustomElement`). `from "react"` is accepted for `useState`, `useEffect`, `useRef` only, as an alias
    of `"tessera"` (other names and `import React` are errors; [`jsx-intrinsics-v1`](jsx-intrinsics-v1/react-compat.md) Q15).
  - `from "./X"` or `from "../X"` imports a component, type or constant exported by another source
    file of the project (extension omitted). Absolute paths and bare package names other than
    `"tessera"` are errors.
  - `import "./file.css";` (side-effect import) registers a stylesheet with the build
    ([`stylesheet-v1.md`](stylesheet-v1.md)); the path must end in `.css`.
- **Exports**: named exports only. `export default` is rejected in V1.
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
- Type aliases are non-recursive and may refer only to earlier or later aliases without cycles.
- Built-in generic names accepted: `Ref<T>` is not written by developers; refs use the element type
  directly (`useRef<Text>(null)`, section 7). Element types are the intrinsic names of section 11.
- Not accepted: `any`, `unknown`, `never`, `object`, `Function`, intersections (`&`), conditional or
  mapped types, `keyof`/`typeof` types, tuple types, enums (`enum`), interfaces (`interface`),
  generics on user declarations, `as` assertions, non-null `!` assertions, `satisfies`, decorators.

## 6. Components

```ebnf
Param  = ( IDENT | DestructuredProps ) ":" TypeExpr ;
DestructuredProps = "{" PropBind { "," PropBind } [ "," ] "}" ;
PropBind = IDENT [ "=" Literal ] ;
Block  = "{" { Stmt } ReturnStmt "}" ;
Stmt   = StateDecl | RefDecl | AnimDecl | EffectStmt | HandlerDecl | LocalDecl ;
ReturnStmt = "return" ( JsxElement | "(" JsxElement ")" ) ";" ;
LocalDecl  = "const" IDENT [ ":" TypeExpr ] "=" PureExpr ";" ;
```

- A component has zero or one parameter: the props. The parameter is either a name
  (`props: CounterProps`, read as `props.initial`) or a destructuring with literal defaults
  (`{ initial = 0 }: CounterProps`). The type annotation is required when the parameter exists.
  Rest elements (`...rest`) are rejected.
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
| Animated value | `const v = useAnimatedValue(init);` | `init` is a number literal or a pure expression of numbers. Use in handlers is `v.set(expr)` and `v.set(expr, { animate: false })`, as in [`animation.md`](../design/animation.md) section 6. Reading the value (`v.value` or any other member) is rejected with `TESSERA_JSX_SUBSET_ANIMATED_READ`; an animated value is passed whole as a prop or style value. The project owner decided this |
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
`onError`. Exactly three forms are accepted.

### 9.1 Setter reference

The name of a state setter: `onChange={setEmail}`. The setter receives the event's primary value (the
`value` of a text event, otherwise the whole payload) and its type must match the state type
([`jsx-intrinsics-v1`](jsx-intrinsics-v1/common-and-events.md) section 3).

### 9.2 Inline setter handler

An arrow function whose body is a sequence of calls to setters and animated-value `.set(...)`, with
pure argument expressions, separated by `;`:

```tsx
<Button onClick={() => { setCount(count + 1); setLast("increment"); }}>Increment</Button>
<Button onClick={() => setCount(count + 1)}>Increment</Button>
```

- Parameters are allowed and typed by the event (section 9.4); they may be used in the arguments.
- No other statements: no `if`, no loops, no declarations, no calls other than setters and `.set`.
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
  never an error, and it does not stop code generation). The project owner decided this.
- The preview body is not checked against the C++ code; keeping them equivalent is the developer's
  responsibility. No tool compares them.
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

## 10. JSX

### 10.1 Elements

```ebnf
JsxElement  = "<" TagName { Attr } ( "/>" | ">" { Child } "</" TagName ">" )
            | "<>" { Child } "</>" ;
TagName     = IDENT ;
Attr        = IDENT { "-" IDENT } [ "=" ( STRING | "{" AttrExpr "}" ) ]
            | "{" "..." SpreadSource "}" ;
Child       = JsxElement | JsxText | "{" ChildExpr "}" ;
```

- Tag names name an intrinsic (section 11), a project component or a library component imported from
  `"tessera"`; components start with an uppercase letter. A lowercase tag (`<div>`, `<span>`) is an
  alias of an intrinsic ([`jsx-intrinsics-v1`](jsx-intrinsics-v1/html-aliases.md),
  [ADR-039](../../decisions/ADR-039-lowercase-html-tags-accepted.md)); one with no mapping is `TESSERA_JSX_SUBSET_HTML_TAG`.
- Member tags (`<A.B>`), namespace tags (`<a:b>`), and a tag that is a variable (dynamic component
  type) are rejected.
- Opening and closing tag names must match. A mismatch is a parse error that names both tags.
- A fragment `<>...</>` is accepted only as a child of another element; its children are placed
  directly in the parent in order. A fragment as the component's returned element is rejected,
  because a component lowers to exactly one root node
  ([`ir-v1.md`](ir-v1.md) section 3).

### 10.2 Attributes

- Value forms: a string literal, `{expr}` where `expr` is a pure expression or a handler (section 9),
  or no value (shorthand for `true`, boolean attributes only).
- Attribute names are identifiers; hyphens are allowed only in `aria-*` names.
- Reserved attribute names, handled by the front end: `key`, `ref`, `className`, `style`, `id`, `role`,
  `aria-*`, and event props (`onClick`, `onChange`, ...). The list is the one in
  [`component-model-v1.md`](component-model-v1.md) section 3.
- `ref={r}` accepts only a name declared by `useRef` whose element type matches the tag.
- `key={expr}` is required on the root element returned by a list-loop callback (section 10.5) and is
  rejected elsewhere unless the element is a direct list item.
- Unknown attributes on an intrinsic are errors (`TESSERA_JSX_SUBSET_UNKNOWN_PROP`). Attributes of a
  project or library component are checked against that component's props type.
- Spread (`{...src}`): allowed only when `src` is the current component's props name or an object
  literal, so that the set of keys is known at compile time. The front end expands the spread in
  attribute order; a later attribute overrides an earlier one with the same name.
- `dangerouslySetInnerHTML` and any attribute whose name is not an identifier (for example
  `data-x`, `xlink:href`) are rejected.

### 10.3 `className` and `style`

- `className` is a string literal or a pure expression of type string. Class names are checked
  against the project stylesheets by the style engine ([`stylesheet-v1.md`](stylesheet-v1.md)); an
  unknown class is a warning, not an error.
- `style` must be an inline object literal with identifier keys in camelCase. The front end converts
  keys to CSS property names (`backgroundColor` to `background-color`). Values are literals or pure
  expressions. Keys must be known at compile time; spread and computed keys are rejected
  (`TESSERA_JSX_SUBSET_STYLE_DYNAMIC`). Properties and values are validated against the compatibility
  matrix in [`stylesheet-v1.md`](stylesheet-v1.md) section 11.
- A number value means pixels for length properties and a unitless number for the others, as in the
  stylesheet spec; a string value is parsed as CSS.

### 10.4 Children and conditional rendering

- **Text**: JSX text follows the standard JSX whitespace rule: lines are trimmed, blank lines are
  removed, remaining lines are joined with one space, and interior spaces are preserved. An explicit
  space is written `{" "}`.
- **Entities** in JSX text and in attribute strings: `&amp; &lt; &gt; &quot; &apos; &nbsp;` and
  numeric `&#N;` / `&#xN;`. Other named entities are errors.
- **Expression child** `{expr}`: a pure expression of type string or number (becomes text), a JSX
  element, a conditional form, or a list loop. Booleans and `null` render nothing.
- **Conditional**: `{cond && <X />}` lowers to the node's `condition`; `{cond ? <X /> : <Y />}` lowers
  to two nodes with complementary conditions; `{cond ? <X /> : null}` is allowed. The condition is a
  pure boolean expression. A condition that is not boolean is `TESSERA_JSX_TYPE_MISMATCH`.
- **Children of a user component.** `<Card>...</Card>` is accepted when `Card` declares
  `children: Children`; the children follow this section and are lowered into the `Component`
  node's `children`. Children on a component without the prop are `TESSERA_JSX_CHILDREN_NOT_ALLOWED`;
  `children=` as an attribute is `TESSERA_JSX_SUBSET_UNKNOWN_PROP`.
- A string or number directly in a position that does not accept text (for example as a child of
  `Scroll` when the element only accepts nodes) is an error from the element specification.

### 10.5 Lists

Only this form is accepted:

```tsx
{rows.map((row, index) => (
  <Row key={row.id} label={row.label} />
))}
```

- The receiver is a list-typed name (prop, state, local, or loop item field) or a top-level array
  constant; the callback has one or two parameters (item, optional index) and its body is a single
  JSX element expression (no block body).
- The returned element has `key={...}` with a pure expression of type string or integer.
- Nesting of dynamic lists is limited to depth 2 ([`ir-v1.md`](ir-v1.md) section 5).
- Other array methods (`filter`, `reduce`, `forEach`, `slice`, `sort`, `concat`, ...) are rejected
  (`TESSERA_JSX_SUBSET_ARRAY_METHOD`). Filtering and sorting happen in the state setter or in
  developer C++.
- Large collections use `VirtualList`/`VirtualGrid` ([`virtualization-v1.md`](virtualization-v1.md)),
  not `.map`.

### 10.6 Event props

An event prop name is `on` followed by an uppercase letter. The value is a handler (section 9). The
set of event names per element is defined by the element specifications; an unknown event name on an
intrinsic is `TESSERA_JSX_SUBSET_UNKNOWN_PROP`.

## 11. Intrinsic elements

JSX tag names that the front end maps to IR node kinds ([`ir-v1.md`](ir-v1.md) section 3). The
prop set of each is in [`jsx-intrinsics-v1`](jsx-intrinsics-v1/README.md) (the last column is the semantic spec).

| Tag | IR kind | Children | Props defined in |
|---|---|---|---|
| `View` | `View` | nodes and text | [`jsx-intrinsics-v1`](jsx-intrinsics-v1/README.md) |
| `Text` | `Text` | text and expressions | [`component-model-v1.md`](component-model-v1.md) |
| `Button` | `Button` | text and nodes | [`component-model-v1.md`](component-model-v1.md) |
| `TextInput`, `TextArea` | same | none | [`text-editing-v1.md`](text-editing-v1.md) |
| `Image` | `Image` | none | section 11.1 |
| `Path` | `Path` | none | section 11.1 |
| `Scroll` | `Scroll` | nodes | [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md) |
| `Spacer` | `Spacer` | none | section 11.1 |
| `VirtualList`, `VirtualGrid` | same | one item template | [`virtualization-v1.md`](virtualization-v1.md) |
| `ErrorBoundary` | `ErrorBoundary` | exactly one child element | [`error-boundaries-v1.md`](error-boundaries-v1.md) |
| `CustomElement` | `CustomElement` | none | [`custom-elements-v1.md`](custom-elements-v1.md) |

Library components (for example `Form`, `Field`, `DataGrid`) are imported from `"tessera"` and are
ordinary components for the front end; their props are validated against their declared prop types
([`component-library.md`](../design/component-library.md)).

### 11.1 `Image`, `Path` and `Spacer`

All three have no children and no event props (`onClick` and the like give `TESSERA_JSX_SUBSET_UNKNOWN_PROP`).
Every attribute value is a literal or a pure expression as stated per attribute. `key` is accepted as on any
element (section 10.5). These props were decided by the project owner.

**`Image`** (raster image from the project's image assets)

| Prop | Type | Required | Rule |
|---|---|---|---|
| `src` | string literal | yes | A project-relative path of a file inside a directory listed in `assets.images` ([`project-config-v1.md`](project-config-v1.md)). The compiler checks that the file exists (`TESSERA_JSX_IMAGE_SRC`). The file must be PNG, JPEG or WebP and within the limits of [`resources.md`](../design/resources.md) section 2.1 (`TESSERA_JSX_IMAGE_FORMAT`, `TESSERA_JSX_IMAGE_SIZE`). Dynamic sources, URLs and expressions are not accepted in V1 |
| `alt` | string, pure expression | yes | The accessible name. The empty string `""` marks the image as decorative: it is left out of the accessibility tree ([`accessibility-v1.md`](accessibility-v1.md)). A missing `alt` is `TESSERA_JSX_IMAGE_ALT` |
| `fit` | `"contain"`, `"cover"`, `"fill"` or `"none"` (literal) | no | Default `"contain"`. `contain`: scale uniformly to fit inside the element box. `cover`: scale uniformly to cover the box and clip the overflow. `fill`: stretch to the box. `none`: natural size, centered, clipped |
| `className`, `style` | as section 10.3 | no | Size, margin, opacity and the other properties of [`stylesheet-v1.md`](stylesheet-v1.md). Without a size, the element is the decoded image size in dp |

**`Path`** (vector path)

| Prop | Type | Required | Rule |
|---|---|---|---|
| `d` | string literal | yes | SVG path data (SVG 1.1 section 8.3). The compiler parses it (`TESSERA_JSX_PATH_DATA` on error); the data is stored in IR as written |
| `viewBox` | string literal `"minX minY width height"` | yes | Four numbers separated by spaces, with width and height greater than 0. The path is scaled uniformly to fit the element box and centered (SVG `preserveAspectRatio="xMidYMid meet"`) |
| `fill` | CSS color literal, `var(--name)` literal, or `"none"` | no | Default `"none"` |
| `stroke` | same as `fill` | no | Default `"none"` |
| `strokeWidth` | number literal | no | In viewBox units. Default `1`. Ignored when `stroke` is `"none"` |
| `label` | string, pure expression | no | The accessible name. Without it the path is decorative and left out of the accessibility tree |
| `className`, `style` | as section 10.3 | no | Same as `Image` |

**`Spacer`** (empty space inside a flex container)

| Prop | Type | Required | Rule |
|---|---|---|---|
| `size` | length (number of dp, or a string with a unit of [`stylesheet-v1.md`](stylesheet-v1.md) section 8.1) | one of `size`, `grow` | Fixed size along the container's main axis (the flex basis) |
| `grow` | number literal, 0 or greater | one of `size`, `grow` | Share of the free main-axis space; default `0` |

A `Spacer` with neither prop is `TESSERA_JSX_SPACER_EMPTY`. It never shrinks, accepts no `className` or
`style`, and is always decorative.

## 12. Rejected constructs

Every rejection is a compile error with a span, a stable code, a one-line message that names the
construct, and, where one exists, a suggested fix. There is no best-effort parsing of rejected code
and no silent skipping.

| Construct | Code | Suggestion |
|---|---|---|
| `class`, `this`, `new`, `extends` | `TESSERA_JSX_SUBSET_CLASS` | Use function components |
| `let`, `var`, reassignment, `++`, `--`, `+=` and other assignment operators | `TESSERA_JSX_SUBSET_MUTATION` | Use `const` and setters |
| `if`, `for`, `while`, `do`, `switch`, `try`, `throw`, labels in a component body | `TESSERA_JSX_SUBSET_STATEMENT` | Use `?:`, `&&`, `.map` in JSX |
| `async`, `await`, `yield`, generators, promises | `TESSERA_JSX_SUBSET_ASYNC` | Use async data primitives ([`async-data-v1.md`](async-data-v1.md)) |
| `import()`, `require`, default import/export, `import * as` | `TESSERA_JSX_SUBSET_IMPORT` | Use named imports |
| Custom hooks | `TESSERA_REACT_CUSTOM_HOOK` | Inline the hook or write a native handler |
| Context, `createContext`, `useContext`, suspense, portals | `TESSERA_REACT_UNSUPPORTED_API` | Use services and overlays ([`overlays-and-layers-v1.md`](overlays-and-layers-v1.md)) |
| `useReducer`, `memo`, `forwardRef`; `useMemo`/`useCallback` outside the safe forms | `TESSERA_REACT_UNSUPPORTED_API` | Compute in a `const`. `useMemo(() => pure expr, deps)` and `useCallback(setter-call arrow, deps)` lower as plain locals/handlers (Q16, `jsx-intrinsics-v1/react-compat.md` 4.2) |
| Lowercase tags with no alias mapping | `TESSERA_JSX_SUBSET_HTML_TAG` | See the alternative in `jsx-intrinsics-v1/html-aliases.md` |
| Dynamic component type, member tags, namespace tags | `TESSERA_JSX_SUBSET_DYNAMIC_TAG` | Use a fixed tag |
| `==`, `!=` | `TESSERA_JSX_SUBSET_LOOSE_EQUALITY` | Use `===`, `!==` |
| `??`, `?.`, `**`, bitwise operators, comma, `typeof`, `instanceof`, `in`, `delete`, `void` | `TESSERA_JSX_SUBSET_OPERATOR` | Not available in V1 |
| Arbitrary calls, method calls other than `.set`/`.map` | `TESSERA_JSX_SUBSET_CALL` | Use an allowlisted function or a native handler |
| Arrow or function values outside section 9 | `TESSERA_JSX_SUBSET_FUNCTION_VALUE` | Declare a handler |
| Object/array literals outside sections 8.4 and 8.5 | `TESSERA_JSX_SUBSET_LITERAL` | Use state or a constant |
| Dynamic `style` keys or spread in `style` | `TESSERA_JSX_SUBSET_STYLE_DYNAMIC` | Use literal keys |
| `dangerouslySetInnerHTML`, `innerHTML`, DOM/window/document access | `TESSERA_JSX_SUBSET_DOM` | Not available |
| Hooks in conditions, loops, handlers or nested functions | `TESSERA_REACT_HOOK_ORDER` | Move to the top of the component |
| State setter called during render | `TESSERA_REACT_SETTER_IN_RENDER` | Call it in a handler or an effect |
| Missing or extra effect dependency | `TESSERA_REACT_EFFECT_DEPS` | Match dependencies exactly |
| Missing key on a list element | `TESSERA_REACT_LIST_KEY` | Add `key` |
| Types in `.jsx`; `any`, `enum`, `interface`, etc. | `TESSERA_JSX_SUBSET_TYPES` | Use the types of section 5 |
| Array methods other than `.map` (`filter`, `reduce`, `forEach`, `slice`, `sort`, ...) | `TESSERA_JSX_SUBSET_ARRAY_METHOD` | Filter or sort in a setter or in developer C++ |
| Bare `number` type | `TESSERA_JSX_TYPE_NUMBER` | Write `int` or `float` (section 5) |
| Reading an animated value (`v.value`) | `TESSERA_JSX_SUBSET_ANIMATED_READ` | Pass the animated value whole (section 7) |
| `return` inside an effect body other than the final cleanup | `TESSERA_REACT_EFFECT_RETURN` | Use the cleanup form (section 7) |
| Unknown `use...` hook name | `TESSERA_REACT_UNKNOWN_HOOK` | Use a core hook or a manifest hook (sections 7 and 7.1) |
| Known hook not in `ui.hooks.allowed` | `TESSERA_REACT_HOOK_NOT_ALLOWED` | Add it to `ui.hooks.allowed`, or remove the call (warning when `ui.hooks.strict` is `false`) |
| Import cycle between files | `TESSERA_JSX_SUBSET_IMPORT_CYCLE` | Move shared code to a third file |
| Malformed or duplicate `bind::` id | `TESSERA_REACT_BIND_ID` | Use `[a-z][a-z0-9_]*`, unique in the project |
| `bind::` annotation in a position other than before a handler or `useEffect` | `TESSERA_REACT_BIND_POSITION` | Move the comment (section 9.3) |
| A hook written in a form other than those of section 7 (for example `const x = useState(0);`) | `TESSERA_REACT_HOOK_FORM` | Use the form of the table in section 7 |
| `const f = () => ...` without `bind::` whose body is not made of setter calls only (section 9.2) | `TESSERA_REACT_BIND_MISSING` | Add `// Note: bind::<id>` before it, or reduce the body to setter calls |
| Member access on a value of unknown record type | `TESSERA_JSX_SUBSET_MEMBER` | Declare a type for the value |
| Unknown attribute or event name on an intrinsic | `TESSERA_JSX_SUBSET_UNKNOWN_PROP` | See the element's spec (section 11) |
| Children on a leaf intrinsic or on a component without a `children: Children` prop | `TESSERA_JSX_CHILDREN_NOT_ALLOWED` | Remove them, or declare the prop (sections 5, 10.4) |
| Operand or argument type mismatch | `TESSERA_JSX_TYPE_MISMATCH` | Convert explicitly or fix the type |
| Exceeding a parser limit | `TESSERA_JSX_LIMIT_*` | Split the file or simplify the expression (section 15) |

## 13. Diagnostics

### 13.1 Format

A diagnostic has: `code`, `severity` (`error` or `warning`), `message`, `span`, optional
`suggestion` (text, and optionally one text edit), and optional related spans (for example the
opening tag for a closing-tag mismatch). Codes are stable strings of the form
`TESSERA_JSX_<AREA>_<NAME>` (lexing, parsing, subset, type) or `TESSERA_REACT_<NAME>` (React-style
rule checks, [`compiler.md`](../design/compiler.md) section 4). A code is never reused for a different
meaning.

Lexing and parsing codes: `TESSERA_JSX_LEX_*` (invalid character, unterminated string/comment/template,
invalid escape, invalid UTF-8, lone CR) and `TESSERA_JSX_PARSE_*` (unexpected token, missing `;`,
mismatched tags, unclosed element). The full list is generated from the source and tracked by the
diagnostics golden tests (section 17), not copied here.

### 13.2 Spans

A span records: project-relative file path with POSIX separators, byte offset and byte length in the
UTF-8 file, and 1-based start/end line and column, where a column counts Unicode code points from the
start of the line. [`ir-v1.md`](ir-v1.md) section 8 defines the same span fields. Byte offsets exist so that the designer's patch writer can edit text exactly
([ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) item 6).

### 13.3 Error count

The front end reports all independent errors it can find in a file, in source order, up to the
diagnostics limit of section 15. Errors caused only by an
earlier error (for example a use of a name whose declaration failed) are not reported.

## 14. Error-tolerant parsing (live view)

The command-line compiler stops generating for a component that has errors. The designer needs to
keep showing the previous result while the developer types, so the parser has a recovery mode with
these rules ([ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) item 3):

- **Recovery unit**: one top-level item. After a parse error inside a top-level item, the parser skips
  tokens until the start of the next top-level item (a line whose first token is `import`, `export`,
  `type`, `function`, or `const` at brace depth 0) and continues.
- **Per-component status**: each component is `Valid` or `Invalid`. An `Invalid` component produces no
  IR; the designer keeps the last IR that was valid for the same `ComponentId`.
- **Isolation**: an error in one component never changes the IR of another component in the same file.
- **Same diagnostics**: recovery mode reports the same codes and spans as the compiler for the same
  input; it only continues after the error. It never fabricates tokens or auto-corrects source.
- **Determinism**: the recovery result depends only on the file contents.
- **Incremental input**: the designer gives the front end the full text of a changed file. Incremental
  re-lexing is an optimization that must produce identical results (section 17).

Recovery mode is not used by production code generation.

## 15. Determinism and limits

- Output depends only on the input text, the project configuration and the manifests; no wall
  clock, random values, absolute paths, environment variables or locale.
- Identical input produces byte-identical AST dumps, IR and diagnostics.
- The front end allocates from bounded arenas and has configurable limits for: file size, token
  count, nesting depth of parentheses/JSX/expressions, number of diagnostics per file, and number of
  components per file. Exceeding a limit is an error with its own code
  (`TESSERA_JSX_LIMIT_*`), never a crash or a hang. Default values, decided by the project owner as
  starting points to be re-measured once the parser exists, and changeable in project configuration:

  | Limit | Default | Code |
  |---|---|---|
  | File size | 1 MiB | `TESSERA_JSX_LIMIT_FILE_SIZE` |
  | Tokens per file | 200000 | `TESSERA_JSX_LIMIT_TOKENS` |
  | Nesting depth (parentheses, JSX elements, expressions) | 128 | `TESSERA_JSX_LIMIT_DEPTH` |
  | Diagnostics per file (reporting stops after this many) | 100 | `TESSERA_JSX_LIMIT_DIAGNOSTICS` |
  | Components per file | 64 | `TESSERA_JSX_LIMIT_COMPONENTS` |

  The configuration keys are `ui.limits.maxFileBytes`, `maxTokens`, `maxNestingDepth`, `maxDiagnostics`
  and `maxComponents` ([`project-config-v1.md`](project-config-v1.md), section `ui`), in the order of the
  table.
- It is portable C++: no operating-system, threading or filesystem calls inside the parser. File
  reading happens in the caller so the same library links into the native CLI and the Wasm module.

## 16. Front-end output

### 16.1 AST contract

The parser outputs a normalized module AST: imports, type aliases, constants, and components with
their props, hook declarations, handler declarations, local constants, and one JSX tree. Every node
carries a span. The AST is internal: it is not serialized as a public format, and code generation
reads only the IR (AGENTS.md Non-negotiables item 4).

### 16.2 Generated typings

For editor support the build generates `.d.ts` files: framework typings for `"tessera"` (intrinsic
props, event objects, built-in hooks, and the type aliases `int` and `float`) and, per project, typings for components and for
`ui/elements.json` ([`custom-elements-v1.md`](custom-elements-v1.md)). Editors use them for completion
and hover; the front end does not read them to decide validity.

## 17. Tests

- **Corpus**: one passing fixture for every grammar production and every accepted form in sections 4
  to 11; one rejection fixture for every row of section 12, each checked for code, span and message.
- **Diagnostics goldens**: the full diagnostic text for each rejection fixture is stored and compared.
- **Determinism**: parse twice and in different directories; AST dumps, IR and diagnostics are
  byte-identical.
- **Fuzzing**: random and mutated inputs must never crash, hang or exceed the limits of section 15;
  they must produce either an AST or diagnostics.
- **Recovery mode**: for each rejection fixture, truncated and edited variants keep the other
  components valid and produce the same diagnostics as the compiler; incremental re-lexing equals a
  full parse.
- **Spans**: every node's span slices the source text to exactly its token range, including with
  CRLF files and non-ASCII text.
- **Subset parity with the designer**: for the corpus, the IR evaluator in the Wasm engine and
  generated C++ give the same scene snapshots
  ([ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) item 4); handlers of section 9.2
  are covered, and native handlers are covered through their preview bodies.
- **Round trip for patches**: a source patch produced by the designer's style editing re-parses to the
  intended IR and leaves unrelated text unchanged.

## 18. Open questions (no decision yet)

Decided by the project owner and recorded in the sections named: no automatic semicolon insertion
(3.6), optional preview body (9.3), `int`/`float` types (5), cleanup mapping (7), no reading of animated
values (7), lowercase HTML tag aliases accepted (10.1, ADR-039) and no `export default` (4 and 12), parser limit defaults and
configuration keys (15), numeric semantics and the pure-function allowlist
([`ir-v1.md`](ir-v1.md) sections 4.1 and 4.2), the hook manifest and its two V1 hooks (7.1), props of
`Image`, `Path` and `Spacer` (11.1), the default theme `default` and the `useThemeState()` hook (7.1), the theme asset format
([`stylesheet-v1.md`](stylesheet-v1.md) section 9.1), the manifest location (7.1), accepted image formats and
limits ([`resources.md`](../design/resources.md) section 2.1).

Still open:

- None.
