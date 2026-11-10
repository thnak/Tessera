# JSX/TSX subset v1: JSX, elements, rejections and tests

Status: proposed. Second part of the normative [JSX/TSX subset v1](jsx-subset-v1.md) (split to
respect the 600-line document limit). Section numbers continue from part 1 (sections 0-9: terms,
lexical grammar, modules, types, components, hooks, expressions, handlers) so every `section N`
reference in the specs and ADRs stays valid.

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

**Stability rule.** A code's spelling and meaning are fixed once it ships: it is never reused for a
different construct, and a reworded message is a golden diff rather than a new code. Adding a code,
renaming one, or narrowing what one means requires, in one change: the code in
[`diagnostic_registry.hpp`](../../src/compiler/analyzer/include/tessera/compiler/jsx/diagnostic_registry.hpp)
(sorted single list of every stable code, including those a later pipeline stage owns), a minimal
fixture in [`jsx_diagnostics_cases.hpp`](../../tests/unit/jsx_diagnostics_cases.hpp) that reports
the code once with its exact byte span, resolved line:column and bounded one-line message (or an
explicit skip entry naming the stage that owns the code), and the byte-compared golden
[`diagnostics-registry.txt`](../../tests/golden/jsx/diagnostics-registry.txt). The registry test
(`jsx_diagnostics_registry`) scans the analyzer and rules sources for emission sites and fails on an
emitted code with no fixture, a fixture for a code nothing emits, a listed code with no fixture and
no skip entry, or golden drift; its failure text says that adding or renaming a code requires
updating the golden.

**Coverage.** A fixture names the entry point that owns its code: the lexer, one of the parser
entry points (expression, JSX, type, module), or the Tier-1 rules, whose fixtures parse a small
module and then run it through [`analyze_module`](../../src/compiler/rules/include/tessera/compiler/rules/analyze.hpp)
so the span and the message are the rules stage's own output. A code that no stage emits yet is an
explicit skip entry in
[`jsx_diagnostics_cases.hpp`](../../tests/unit/jsx_diagnostics_cases.hpp) that names its owner and
the reason; the test fails as soon as a skipped code starts being emitted, so the skip cannot hide
a new emission site. The skips today are `TESSERA_JSX_CHILDREN_NOT_ALLOWED` (the intrinsic
element-constraint evaluation in `src/schema/`; the code is only a `ConstraintRow` code string, no
pass emits it), `TESSERA_JSX_SUBSET_ANIMATED_READ` and `TESSERA_REACT_SETTER_IN_RENDER` (later
analyzer slices), `TESSERA_JSX_SUBSET_IMPORT_CYCLE` (project resolver),
`TESSERA_JSX_TYPE_MISMATCH` (type/lowering) and `TESSERA_REACT_HOOK_NOT_ALLOWED` (the hook
manifest). The test's summary line prints the live counts of registry codes, fixtures, skips and
emitted codes.

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
  The code registry (section 13.1) additionally pins one minimal fixture per stable code — code,
  span, line:column and bounded message — and compares `tests/golden/jsx/diagnostics-registry.txt`
  byte for byte.
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
